#include "app/glib_poll_source.h"
#include "browser/browser_controller.h"
#include "browser/location_presentation.h"
#include "fs/trash_info.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <thread>

namespace {
  namespace fs = std::filesystem;
  void require(bool condition, const std::string& message) {
    if (!condition)
      throw std::runtime_error(message);
  }
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!done()) {
        require(std::chrono::steady_clock::now() < deadline, "trash browsing timed out");
        std::vector<pollfd> fds;
        const auto start = source.addPollFds(fds);
        require(::poll(fds.data(), fds.size(), 5) >= 0, "poll failed");
        source.dispatch(fds, start);
      }
    }
    void ready(const files::BrowserController& browser) {
      until([&] { return browser.model().snapshot().status != files::DirectoryStatus::LOADING; });
      const auto& snapshot = browser.model().snapshot();
      require(snapshot.status == files::DirectoryStatus::READY, snapshot.error ? snapshot.error->message : "not ready");
    }

  private:
    GlibPollSource source;
  };
  struct Fixture {
    fs::path root;
    Fixture() {
      const files::GString path(g_dir_make_tmp("noctalia-trash-browse-XXXXXX", nullptr));
      require(path != nullptr, "fixture failed");
      root = path.get();
    }
    ~Fixture() {
      std::error_code error;
      fs::remove_all(root, error);
    }
  };
  void metadataAndPresentation() {
    files::GioPtr<GFileInfo> info(g_file_info_new());
    require(
        !files::readTrashInfo(info.get()).original && !files::readTrashInfo(info.get()).deletionDate,
        "missing metadata became known"
    );
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_TRASH_ORIG_PATH, "/wrong-type");
    g_file_info_set_attribute_byte_string(info.get(), G_FILE_ATTRIBUTE_TRASH_DELETION_DATE, "wrong-type");
    require(
        !files::readTrashInfo(info.get()).original && !files::readTrashInfo(info.get()).deletionDate,
        "incorrect metadata types became known"
    );
    g_file_info_set_attribute_byte_string(info.get(), G_FILE_ATTRIBUTE_TRASH_ORIG_PATH, "relative/name");
    require(!files::readTrashInfo(info.get()).original, "relative original path accepted");
    const auto raw = std::string("/tmp/文 #/raw-") + static_cast<char>(0xff);
    g_file_info_set_attribute_byte_string(info.get(), G_FILE_ATTRIBUTE_TRASH_ORIG_PATH, raw.c_str());
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_TRASH_DELETION_DATE, "2026-10-29T12:34:56");
    const auto value = files::readTrashInfo(info.get());
    require(
        value.original && value.original->nativePath() == raw && value.deletionDate == "2026-10-29T12:34:56",
        "raw original path or deletion date changed"
    );
    require(files::trashOriginalParentText(value) == "/tmp/文 #", "original display path lost context");
    const auto root = files::Location::trash();
    require(root.isTrash() && !root.nativePath(), "trash exposed a native mutation address");
    const auto nested = files::Location::fromUri("trash:///a%20b/c%23.txt");
    const auto crumbs = files::locationBreadcrumbs(nested);
    require(
        crumbs.size() == 3
            && crumbs.front().label == "Trash"
            && crumbs.front().location == root
            && crumbs[1].label == "a b"
            && crumbs.back().label == "c#.txt"
            && crumbs.back().location == nested,
        "trash breadcrumb labels changed operation addresses"
    );
    require(files::locationEditText(nested) == nested.uri(), "trash editor exposed a backing path");
  }
  void unavailableProvider() {
    Pump pump;
    files::BrowserController browser({}, false);
    browser.open(files::Location::trash());
    pump.until([&] { return browser.model().snapshot().status == files::DirectoryStatus::ERROR; });
    require(
        browser.model().snapshot().error->kind == files::FileErrorKind::UNSUPPORTED && browser.model().rows().empty(),
        "unavailable trash presented a successful empty listing"
    );
    browser.refresh();
    pump.until([&] { return browser.model().snapshot().status == files::DirectoryStatus::ERROR; });
    Fixture fixture;
    browser.open(files::Location::fromPath(fixture.root.native()));
    pump.ready(browser);
    require(browser.back(), "trash was lost from history");
    pump.until([&] { return browser.model().snapshot().status == files::DirectoryStatus::ERROR; });
  }
  void concurrentTrashLocations() {
    std::barrier gate(4);
    std::atomic<bool> valid{true};
    std::array<std::jthread, 4> workers;
    for (auto& worker : workers) {
      worker = std::jthread([&] {
        gate.arrive_and_wait();
        for (int iteration = 0; iteration < 10000; ++iteration) {
          const auto root = files::Location::trash();
          const auto nested = files::Location::fromUri("trash:///parallel%20folder/item%23.txt");
          const auto file = root.file();
          const files::GString uri(g_file_get_uri(file.get()));
          if (!root.isTrash() || nested.nativePath() || !nested.isTrash() || root.uri() != uri.get())
            valid.store(false);
        }
      });
    }
    for (auto& worker : workers)
      worker.join();
    require(valid.load(), "concurrent Trash URI construction changed identity");
  }
  void providerBrowsing() {
    Fixture fixture;
    const auto a = fixture.root / "first";
    const auto b = fixture.root / "second";
    fs::create_directories(a / "Folder 文");
    fs::create_directory(b);
    std::ofstream(a / "same #%.txt") << "first";
    std::ofstream(b / "same #%.txt") << "second";
    std::ofstream(a / "Folder 文/inside.txt") << "child";
    std::ofstream(a / ".hidden") << "hidden";
    const auto raw = std::string("raw-") + static_cast<char>(0xff);
    std::ofstream(a / raw) << "raw";
    for (const auto& path : {a / "same #%.txt", b / "same #%.txt", a / "Folder 文", a / ".hidden", a / raw}) {
      GError* error = nullptr;
      const bool ok = g_file_trash(files::Location::fromPath(path.native()).file().get(), nullptr, &error);
      const std::string message = error ? error->message : "trash preparation failed";
      g_clear_error(&error);
      require(ok, message);
    }
    Pump pump;
    files::BrowserController browser({}, false);
    bool launched = false;
    browser.setFileActivatedCallback([&](const auto&) { launched = true; });
    browser.open(files::Location::trash());
    pump.ready(browser);
    const auto& entries = browser.model().snapshot().entries;
    require(entries.size() == 5, "provider omitted trashed items");
    require(
        std::ranges::count(entries, "same #%.txt", &files::DirectoryEntry::displayName) == 2,
        "trash collisions lost original display names"
    );
    const auto first = std::ranges::find_if(entries, [&](const auto& entry) {
      return entry.trash.original == files::Location::fromPath((a / "same #%.txt").native());
    });
    require(
        first != entries.end()
            && first->trash.deletionDate
            && first->location.isTrash()
            && first->parentDisplayName == a.native(),
        "provider metadata lost operation/display separation"
    );
    const auto id = first->id;
    const auto operation = first->location;
    browser.model().select(id);
    browser.activate(id);
    require(!launched && browser.model().snapshot().location == files::Location::trash(), "trash file launched");
    browser.refresh();
    pump.ready(browser);
    require(
        browser.model().selectedIds().empty()
            && std::ranges::any_of(
                browser.model().snapshot().entries, [&](const auto& entry) { return entry.location == operation; }
            ),
        "unmonitored refresh retargeted a stale selection or lost the item"
    );
    browser.toggleHidden();
    require(browser.model().rows().size() == 5, "hidden trash item lost");
    const auto folder =
        std::ranges::find(browser.model().snapshot().entries, "Folder 文", &files::DirectoryEntry::displayName);
    require(folder != browser.model().snapshot().entries.end(), "trashed folder missing");
    browser.activate(folder->id);
    pump.ready(browser);
    require(
        browser.model().rows().size() == 1 && browser.model().snapshot().entries.front().displayName == "inside.txt",
        "trashed folder browsing failed"
    );
    require(browser.up(), "trash Up failed");
    pump.ready(browser);
    require(
        browser.model().snapshot().location == files::Location::trash() && !browser.canUp(),
        "trash root ancestry escaped"
    );
    browser.setSearchQuery("inside");
    pump.ready(browser);
    require(
        browser.model().rows().size() == 1 && browser.model().snapshot().entries.front().location.isTrash(),
        "recursive trash search lost its source"
    );
    browser.activate(browser.model().rows().front());
    require(!launched, "trash search result launched");
    browser.setSearchQuery({});
    browser.open(files::Location::fromPath(fixture.root.native()));
    pump.ready(browser);
    require(browser.back(), "trash history failed");
    pump.ready(browser);
    require(browser.model().snapshot().location == files::Location::trash(), "history retargeted trash");
  }
} // namespace

int main(int argc, char** argv) {
  try {
    metadataAndPresentation();
    if (argc == 2 && std::string_view(argv[1]) == "--uri-stress")
      concurrentTrashLocations();
    else if (argc == 2 && std::string_view(argv[1]) == "--provider")
      providerBrowsing();
    else {
      require(argc == 1, "unexpected test arguments");
      unavailableProvider();
      concurrentTrashLocations();
    }
    std::println("trash-browse checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "trash-browse: {}", error.what());
    return 1;
  }
}
