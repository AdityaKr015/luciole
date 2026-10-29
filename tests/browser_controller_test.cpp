#include "app/glib_poll_source.h"
#include "browser/browser_controller.h"
#include "browser/location_presentation.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>

namespace {
  using namespace std::chrono_literals;
  namespace fs = std::filesystem;
  using files::BrowserController;
  using files::DirectoryStatus;
  using files::EntryId;
  using files::Location;

  void require(bool condition, const std::string& message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  struct Fixture {
    fs::path path;
    Fixture() {
      const files::GString name(g_dir_make_tmp("noctalia-browser-test-XXXXXX", nullptr));
      require(name != nullptr, "cannot create browser fixture");
      path = name.get();
      fs::create_directory(path / "child");
      fs::create_directory(path / "other");
      std::ofstream(path / "file") << "fixture";
      std::ofstream(path / "child" / "inside") << "child fixture";
      std::ofstream(path / ".hidden") << "hidden";
      fs::create_symlink("child", path / "folder-link");
    }
    ~Fixture() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
  };

  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!done()) {
        require(std::chrono::steady_clock::now() < deadline, "browser navigation timed out");
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        require(::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) >= 0, "poll failed");
        m_source.dispatch(fds, start);
      }
    }
    void ready(const BrowserController& browser) {
      until([&] { return browser.model().snapshot().status != DirectoryStatus::LOADING; });
      require(
          browser.model().snapshot().status == DirectoryStatus::READY,
          browser.model().snapshot().error ? browser.model().snapshot().error->message : "browser not ready"
      );
    }

  private:
    GlibPollSource m_source;
  };

  EntryId id(const BrowserController& browser, const std::string& name) {
    for (const auto& item : browser.model().snapshot().entries) {
      if (item.name == name) {
        return item.id;
      }
    }
    throw std::runtime_error("missing browser fixture entry: " + name);
  }

  void testNavigation() {
    Fixture fixture;
    Pump pump;
    BrowserController browser({}, false);
    const auto root = Location::fromPath(fixture.path.native());
    require(browser.openText(root.uri()), "URI navigation rejected");
    pump.ready(browser);
    require(!browser.canBack() && !browser.canForward(), "initial navigation created extra history");
    browser.model().select(id(browser, "file"));
    const auto selected = browser.model().selectedIds();
    browser.open(*browser.model().snapshot().location);
    pump.ready(browser);
    require(
        browser.model().selectedIds() == selected && !browser.canBack(), "same-location reload lost identity/history"
    );
    browser.activate(id(browser, "child"));
    pump.ready(browser);
    require(
        browser.model().snapshot().location == Location::fromPath((fixture.path / "child").native()),
        "folder activation failed"
    );
    require(browser.openText("../other"), "relative navigation rejected");
    pump.ready(browser);
    require(
        browser.model().snapshot().location == Location::fromPath((fixture.path / "other").native()),
        "relative path resolved against wrong directory"
    );
    require(browser.back(), "back unavailable");
    pump.ready(browser);
    require(
        browser.model().snapshot().location == Location::fromPath((fixture.path / "child").native())
            && browser.canForward(),
        "back did not restore history"
    );
    require(browser.forward(), "forward unavailable");
    pump.ready(browser);
    require(browser.up(), "up unavailable");
    pump.ready(browser);
    require(browser.model().snapshot().location == root, "parent navigation changed target");
    require(browser.back(), "cannot move back before branching");
    pump.ready(browser);
    browser.open(root);
    pump.ready(browser);
    require(!browser.canForward(), "new navigation kept an abandoned forward branch");
    browser.activate(id(browser, "folder-link"));
    pump.ready(browser);
    require(
        browser.model().snapshot().location == Location::fromPath((fixture.path / "folder-link").native()),
        "folder symlink navigation failed"
    );
    browser.activate(id(browser, "inside"));
    require(
        !browser.message().empty() && browser.model().snapshot().status == DirectoryStatus::READY,
        "file activation navigated as a folder"
    );
  }

  void testFailureAndLiveUpdates() {
    Fixture fixture;
    Pump pump;
    BrowserController browser({}, false);
    browser.open(Location::fromPath(fixture.path.native()));
    pump.ready(browser);
    const auto location = browser.model().snapshot().location;
    require(
        !browser.openText("sftp://invalid%zz") && browser.model().snapshot().location == location,
        "invalid input replaced the current location"
    );
    browser.toggleHidden();
    const auto hidden = id(browser, ".hidden");
    browser.model().select(hidden);
    browser.toggleHidden();
    require(browser.model().selection().selected.empty(), "hidden selected entry was retained");
    browser.activate(hidden);
    require(browser.model().snapshot().location == location, "activation accepted a filtered ID");
    browser.refresh();
    pump.ready(browser);
    const auto file = id(browser, "file");
    browser.model().select(file);
    std::ofstream(fixture.path / "added") << "new file";
    pump.until([&] { return browser.model().snapshot().entries.size() == 6; });
    require(browser.model().selection().selected.contains(file), "monitor update lost stable selection");
    require(browser.openText((fixture.path / "missing").native()), "valid missing path rejected synchronously");
    pump.until([&] { return browser.model().snapshot().status == DirectoryStatus::ERROR; });
    require(browser.canBack() && browser.back(), "failed navigation could not be left with Back");
    pump.ready(browser);
    browser.stop();
    require(browser.model().snapshot().status == DirectoryStatus::CANCELLED, "stop did not cancel the browser session");
    browser.refresh();
    pump.ready(browser);
  }
  void testRecursiveSearch() {
    Fixture fixture;
    Fixture outside;
    fs::create_directories(fixture.path / "child/deep");
    fs::create_directory(fixture.path / ".private");
    fs::create_directory(fixture.path / "target-folder");
    for (const auto& name : {"target.txt", "child/target.txt", "child/deep/target.txt", ".private/target.txt"}) {
      std::ofstream(fixture.path / name) << "target";
    }
    std::ofstream(outside.path / "outside-target.txt") << "outside";
    std::ofstream(fixture.path / "Straße.txt") << "Unicode";
    std::ofstream(fixture.path / "*.txt") << "literal";
    fs::create_directory_symlink(outside.path, fixture.path / "external-link");
    fs::create_directory_symlink(".", fixture.path / "target-link");
    Pump pump;
    BrowserController browser({}, false);
    const auto root = Location::fromPath(fixture.path.native());
    browser.open(root);
    pump.ready(browser);
    const auto oldId = id(browser, "target.txt");
    browser.model().select(oldId);
    browser.setSearchQuery("TARGET");
    pump.ready(browser);
    require(browser.model().rows().size() == 5, "recursive search missed nested files or followed hidden/link folders");
    require(browser.model().selection().selected.empty(), "search retargeted an old browsing selection");
    std::unordered_set<std::string> targets;
    for (const auto& entry : browser.model().snapshot().entries) {
      targets.insert(entry.location.uri());
      require(!entry.parentDisplayName.empty() && entry.id != oldId, "result lost parent context or source identity");
      if (entry.type == G_FILE_TYPE_REGULAR && !entry.symlink)
        require(entry.contentType == "text/plain", "recursive results lost fast content-type metadata");
    }
    require(
        targets.contains(Location::fromPath((fixture.path / "child/deep/target.txt").native()).uri())
            && !targets.contains(Location::fromPath((outside.path / "outside-target.txt").native()).uri()),
        "search used wrong operation locations or traversed an external symlink"
    );
    browser.setSearchQuery("STRASSE");
    pump.ready(browser);
    require(
        browser.model().rows().size() == 1 && browser.model().snapshot().entries.front().name == "Straße.txt",
        "recursive query lost Unicode matching"
    );
    browser.setSearchQuery("*");
    pump.ready(browser);
    require(
        browser.model().rows().size() == 1 && browser.model().snapshot().entries.front().name == "*.txt",
        "recursive query interpreted a glob"
    );
    browser.setSearchQuery("obsolete");
    browser.setSearchQuery("target");
    browser.setSearchQuery("STRASSE");
    pump.ready(browser);
    require(
        browser.model().rows().size() == 1 && browser.searchQuery() == "STRASSE",
        "late query results replaced current search"
    );
    browser.setSearchQuery("target");
    browser.stop();
    require(browser.model().snapshot().status == DirectoryStatus::CANCELLED, "search stop was ignored");
    browser.refresh();
    pump.ready(browser);
    require(
        browser.model().rows().size() == 5 && browser.searchQuery() == "target", "search refresh lost query/results"
    );
    browser.toggleHidden();
    pump.ready(browser);
    require(browser.model().rows().size() == 6, "hidden search did not include hidden descendants");
    require(!browser.openText("sftp://invalid%zz") && browser.searchQuery() == "target", "invalid path cleared query");
    browser.setSearchQuery({});
    require(
        browser.model().snapshot().location == root
            && browser.model().selection().selected.empty()
            && id(browser, "target.txt") == oldId,
        "clearing search did not restore the browsing source safely"
    );
    browser.setSearchQuery("target");
    pump.ready(browser);
    browser.open(root);
    pump.ready(browser);
    require(browser.searchQuery().empty() && !browser.canBack(), "reopening the search root did not exit search");
    browser.setSearchQuery("target");
    pump.ready(browser);
    const auto directory = id(browser, "target-folder");
    browser.activate(directory);
    pump.ready(browser);
    require(
        browser.searchQuery().empty() && browser.model().snapshot().entries.empty(), "folder result activation failed"
    );
    browser.setSearchQuery("missing");
    require(browser.back(), "Back from search failed");
    pump.ready(browser);
    require(browser.searchQuery().empty() && browser.model().snapshot().location == root, "history retained search");

    for (int i = 0; i < 180; ++i) {
      std::ofstream(fixture.path / ("batch-match-" + std::to_string(i))) << "batch";
    }
    bool progressive = false;
    bool selectable = false;
    browser.setChangedCallback([&] {
      if (!progressive
          && browser.model().snapshot().status == DirectoryStatus::LOADING
          && !browser.model().rows().empty()) {
        progressive = true;
        selectable = browser.model().select(browser.model().rows().front());
      }
    });
    browser.setSearchQuery("batch-match");
    pump.ready(browser);
    browser.setChangedCallback({});
    require(
        progressive
            && selectable
            && browser.model().rows().size() == 180
            && browser.model().selection().selected.size() == 1,
        "search did not publish progressive batches with stable selection"
    );
    browser.setSearchQuery("target");
    browser.open(Location::fromPath(outside.path.native()));
    pump.ready(browser);
    require(
        browser.searchQuery().empty()
            && browser.model().snapshot().location == Location::fromPath(outside.path.native()),
        "late search polluted navigation"
    );
    browser.setSearchQuery("target");
    pump.ready(browser);
    require(browser.model().rows().size() == 1, "new search used the previous root");
    require(browser.openText((fixture.path / "missing").native()), "missing path rejected synchronously");
    pump.until([&] { return browser.model().snapshot().status == DirectoryStatus::ERROR; });
    browser.setSearchQuery("target");
    pump.until([&] { return browser.model().snapshot().status == DirectoryStatus::ERROR; });
    require(
        browser.model().snapshot().error && browser.model().snapshot().error->kind == files::FileErrorKind::MISSING,
        "search root failure lost its diagnostic"
    );
  }

  void testSearchLifetimeAndPartialFailure() {
    Fixture fixture;
    Pump pump;
    auto immediate = std::make_unique<files::RecursiveSearch>();
    immediate->setChangedCallback([&] { immediate.reset(); });
    immediate->start(Location::fromPath(fixture.path.native()), "file", false);
    require(!immediate, "search observer did not destroy its owner during initial publication");
    if (::geteuid() != 0) {
      const auto denied = fixture.path / "denied";
      fs::create_directory(denied);
      std::ofstream(denied / "inside-target") << "inaccessible";
      fs::permissions(denied, fs::perms::none);
      BrowserController browser({}, false);
      browser.open(Location::fromPath(fixture.path.native()));
      pump.ready(browser);
      browser.setSearchQuery("file");
      pump.ready(browser);
      fs::permissions(denied, fs::perms::owner_all);
      require(
          browser.model().rows().size() == 1 && browser.skippedSearchFolders() == 1,
          "unreadable descendant hid accessible results or reported complete coverage"
      );
    }

    auto search = std::make_unique<files::RecursiveSearch>();
    bool destroyed = false;
    search->setChangedCallback([&] {
      if (!search->snapshot().entries.empty()) {
        search.reset();
        destroyed = true;
      }
    });
    search->start(Location::fromPath(fixture.path.native()), "file", false);
    pump.until([&] { return destroyed; });
    // Drain cancelled callbacks while a new independent search runs.
    BrowserController browser({}, false);
    browser.open(Location::fromPath(fixture.path.native()));
    pump.ready(browser);
    browser.setSearchQuery("inside");
    pump.ready(browser);
    require(!browser.model().rows().empty(), "search failed after observer destruction");
  }

  void testLocationPresentation() {
    const auto path = Location::fromPath("/tmp/space here/<literal>/café");
    const auto crumbs = files::locationBreadcrumbs(path);
    require(
        crumbs.size() == 5
            && crumbs.front().location == Location::fromPath("/")
            && crumbs.back().location == path
            && crumbs[3].label == "<literal>",
        "breadcrumbs lost ancestors, literal labels or original targets"
    );
    require(files::locationEditText(path) == "/tmp/space here/<literal>/café", "native editor showed URI escapes");
    const auto raw = Location::fromPath("/tmp/raw-" + std::string(1, static_cast<char>(0xff)));
    require(
        files::locationEditText(raw) == raw.uri() && files::locationBreadcrumbs(raw).back().location == raw,
        "non-UTF8 breadcrumb/editor lost operation identity"
    );
    const auto remote = Location::fromUri("sftp://user:secret@example.org/space%20here/leaf");
    const auto remoteCrumbs = files::locationBreadcrumbs(remote);
    require(
        remoteCrumbs.back().location == remote
            && remoteCrumbs.front().label == "example.org"
            && files::locationEditText(remote) == remote.uri(),
        "remote breadcrumb root/editor was incorrect"
    );
  }

} // namespace

int main() {
  try {
    testNavigation();
    testFailureAndLiveUpdates();
    testRecursiveSearch();
    testSearchLifetimeAndPartialFailure();
    testLocationPresentation();
    std::println("browser controller checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "browser controller test: {}", error.what());
    return 1;
  }
}
