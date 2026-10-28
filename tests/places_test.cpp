#include "app/glib_poll_source.h"
#include "browser/places.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>

namespace {
  namespace fs = std::filesystem;
  void require(bool value, const char* message) {
    if (!value) {
      throw std::runtime_error(message);
    }
  }
  struct Fixture {
    fs::path root;
    Fixture() {
      const files::GString path(g_dir_make_tmp("noctalia-places-XXXXXX", nullptr));
      require(path != nullptr, "cannot create Places fixture");
      root = path.get();
      fs::create_directories(root / "home");
      fs::create_directories(root / "config");
      g_setenv("HOME", (root / "home").c_str(), TRUE);
      g_setenv("XDG_CONFIG_HOME", (root / "config").c_str(), TRUE);
      std::ofstream(root / "config/user-dirs.dirs")
          << "XDG_DESKTOP_DIR=\"$HOME\"\nXDG_DOCUMENTS_DIR=\"$HOME/My Documents\"\n"
             "XDG_MUSIC_DIR=\"$HOME/My Documents\"\nXDG_PICTURES_DIR=\"$HOME/Pictures <literal>\"\n"
             "XDG_VIDEOS_DIR=\"$HOME/Missing videos\"\nXDG_PUBLICSHARE_DIR=\"$HOME\"\n"
             "XDG_TEMPLATES_DIR=\"$HOME/Templates\"\nXDG_DOWNLOAD_DIR=\""
          << (root / "outside downloads").native()
          << "\"\n";
    }
    ~Fixture() { fs::remove_all(root); }
    files::Location location(const std::string& name) const {
      return files::Location::fromPath((root / name).native());
    }
    files::Bookmark pin(const std::string& name) const { return {name, location(name)}; }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!done()) {
        require(std::chrono::steady_clock::now() < deadline, "Pins worker timed out");
        std::vector<pollfd> fds;
        const auto start = source.addPollFds(fds);
        const int timeout = source.pollTimeoutMs();
        require(::poll(fds.data(), fds.size(), timeout < 0 ? 20 : std::min(timeout, 20)) >= 0, "poll failed");
        source.dispatch(fds, start);
      }
    }
    void ready(const files::PinService& service) {
      until([&] { return !service.busy(); });
      require(service.editable(), "Pins did not load a valid snapshot");
    }

  private:
    GlibPollSource source;
  };
  void testPlacesAndPins() {
    Fixture fixture;
    Pump pump;
    const auto path = fixture.root / "state.toml";
    files::StateStore state(path);
    require(state.load(), "cannot load empty state");
    const std::vector<files::SavedWindow> windows{
        {.tabs = {{.primary = fixture.location("home"), .secondary = std::nullopt}}}
    };
    require(state.saveWindows(windows) == files::AtomicWriteState::DURABLE, "cannot seed independent windows section");
    files::PinService service(path);
    service.reload();
    pump.ready(service);
    const auto& places = service.userPlaces();
    require(
        places.size() == 7
            && places.front().bookmark.label == "Home"
            && places.back().bookmark.location == files::Location::fromPath("/"),
        "Places duplicated disabled/home directories or lost fixed destinations"
    );
    const auto documents =
        std::ranges::find_if(places, [](const auto& place) { return place.bookmark.label == "Documents"; });
    const auto downloads =
        std::ranges::find_if(places, [](const auto& place) { return place.bookmark.label == "Downloads"; });
    const auto videos =
        std::ranges::find_if(places, [](const auto& place) { return place.bookmark.label == "Videos"; });
    require(
        documents != places.end() && documents->bookmark.location == fixture.location("home/My Documents"),
        "XDG HOME expansion lost its target"
    );
    require(
        downloads != places.end() && downloads->bookmark.location == fixture.location("outside downloads"),
        "external XDG directory was replaced with a hardcoded path"
    );
    require(
        videos != places.end() && !fs::exists(fixture.root / "home/Missing videos"),
        "missing configured folders were omitted or created"
    );

    require(service.add({fixture.pin("a"), fixture.pin("b"), fixture.pin("c")}, 0), "cannot add pins");
    pump.ready(service);
    require(service.error().empty() && service.bookmarks().size() == 3, "pin save failed");
    require(
        service.add({{"replacement label", fixture.location("b")}, fixture.pin("b")}, 0),
        "duplicate pin drop was rejected"
    );
    pump.ready(service);
    require(
        service.bookmarks().size() == 3 && service.bookmarks()[0] == fixture.pin("b"),
        "drop duplicated a pin or lost its label"
    );
    require(service.move(fixture.location("c"), 0), "pin move was rejected");
    pump.ready(service);
    require(
        service.bookmarks()[0] == fixture.pin("c") && service.bookmarks()[1] == fixture.pin("b"),
        "pin order was not saved"
    );
    require(service.removeMany({fixture.location("a"), fixture.location("b")}), "batch unpin was rejected");
    pump.ready(service);
    require(service.bookmarks() == std::vector{fixture.pin("c")}, "batch unpin dropped the wrong targets");
    require(
        state.load() && state.state().windows == windows && state.state().bookmarks == service.bookmarks(),
        "pins overwrote unrelated saved windows"
    );

    require(service.add({{"line\nbreak", fixture.location("controls")}}, 1), "cannot add a literal filename pin");
    pump.ready(service);
    require(service.bookmarks()[1].label == "line break", "pin label retained a forbidden control character");
    const auto before = service.bookmarks();
    require(
        service.add({{"private", files::Location::fromUri("sftp://user:secret@example.org/folder")}}, 2),
        "async validation was not started"
    );
    pump.ready(service);
    require(
        !service.error().empty() && !service.error().contains("secret") && service.bookmarks() == before,
        "credential-bearing pins were stored or leaked in diagnostics"
    );

    files::StateStore foreign(path);
    require(foreign.load(), "cannot load external writer");
    auto external = foreign.state().bookmarks;
    external.push_back(fixture.pin("external"));
    require(foreign.saveBookmarks(external) == files::AtomicWriteState::DURABLE, "cannot seed a stale-writer conflict");
    require(service.add({fixture.pin("not-committed")}, 0), "conflict check was not started");
    pump.ready(service);
    require(
        !service.error().empty()
            && service.bookmarks() == external
            && !service.contains(fixture.location("not-committed")),
        "stale pin save overwrote another writer or hid its conflict"
    );
    require(service.add({fixture.pin("retry")}, 0), "pin edit did not recover after conflict reload");
    pump.ready(service);
    require(service.error().empty() && service.bookmarks().front() == fixture.pin("retry"), "conflict retry failed");

    std::ofstream(path) << "schema_version = 999\n";
    service.reload();
    pump.until([&] { return !service.busy(); });
    require(
        !service.editable() && !service.error().empty() && !service.add({fixture.pin("unsafe")}, 0),
        "invalid saved state allowed a write"
    );
    std::ifstream invalid(path);
    require(
        std::string((std::istreambuf_iterator<char>(invalid)), {}) == "schema_version = 999\n",
        "invalid state was silently replaced"
    );

    auto dying = std::make_unique<files::PinService>(fixture.root / "other/state.toml");
    bool destroyed = false;
    dying->setChangedCallback([&] {
      if (!dying->busy()) {
        dying.reset();
        destroyed = true;
      }
    });
    dying->reload();
    pump.until([&] { return destroyed; });
  }
} // namespace

int main() {
  try {
    testPlacesAndPins();
    std::println("Places and pin checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "Places test: {}", error.what());
    return 1;
  }
}
