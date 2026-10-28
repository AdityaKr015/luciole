#include "app/glib_poll_source.h"
#include "browser/browser_controller.h"

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
  void testCurrentFolderFilter() {
    Fixture fixture;
    std::ofstream(fixture.path / "Straße.txt") << "Unicode";
    std::ofstream(fixture.path / "*.txt") << "literal";
    Pump pump;
    BrowserController browser({}, false);
    browser.open(Location::fromPath(fixture.path.native()));
    pump.ready(browser);
    const auto generation = browser.model().snapshot().generation;
    const auto count = browser.model().snapshot().entries.size();
    const auto file = id(browser, "file");
    browser.model().select(file);
    browser.model().select(id(browser, "child"), files::SelectionMode::TOGGLE);
    browser.setNameFilter("FILE");
    require(
        browser.model().rows() == std::vector<EntryId>{file}
            && browser.model().selectedIds() == std::vector<EntryId>{file},
        "filter did not prune excluded selection or preserve matching identity"
    );
    require(
        browser.model().snapshot().generation == generation && browser.model().snapshot().entries.size() == count,
        "filter rescanned or removed source entries"
    );
    browser.setNameFilter({});
    require(browser.model().selectedIds() == std::vector<EntryId>{file}, "clearing restored excluded selection");
    browser.setNameFilter("STRASSE");
    require(browser.model().rows() == std::vector<EntryId>{id(browser, "Straße.txt")}, "filter lost Unicode matching");
    browser.setNameFilter("*");
    require(browser.model().rows() == std::vector<EntryId>{id(browser, "*.txt")}, "filter interpreted a glob");
    browser.open(*browser.model().snapshot().location);
    pump.ready(browser);
    require(browser.model().filter().text == "*" && !browser.canBack(), "same-location refresh cleared filter/history");
    std::ofstream(fixture.path / "another*.txt") << "monitored";
    pump.until([&] { return browser.model().rows().size() == 2; });
    require(browser.model().filter().text == "*", "monitor update cleared filter");
    browser.setNameFilter("hidden");
    require(browser.model().rows().empty(), "name filter bypassed hidden-file visibility");
    browser.toggleHidden();
    require(browser.model().rows() == std::vector<EntryId>{id(browser, ".hidden")}, "hidden toggle lost name filter");
    require(
        !browser.openText("sftp://invalid%zz") && browser.model().filter().text == "hidden",
        "invalid navigation cleared the current filter"
    );
    browser.stop();
    browser.refresh();
    pump.ready(browser);
    require(browser.model().filter().text == "hidden", "cancel/resume lost filter");
    require(browser.openText("child"), "cannot navigate out of filtered folder");
    pump.ready(browser);
    require(
        browser.model().filter().text.empty()
            && browser.model().filter().showHidden
            && browser.model().rows().size() == 1,
        "new folder inherited text filter or lost hidden preference"
    );
    browser.setNameFilter("inside");
    require(browser.back(), "filtered history Back failed");
    pump.ready(browser);
    require(browser.model().filter().text.empty(), "Back inherited text filter");
    browser.setNameFilter("file");
    require(browser.forward(), "filtered history Forward failed");
    pump.ready(browser);
    require(browser.model().filter().text.empty(), "Forward inherited text filter");
    browser.setNameFilter("inside");
    require(browser.up(), "filtered parent navigation failed");
    pump.ready(browser);
    require(browser.model().filter().text.empty(), "parent navigation inherited text filter");
  }
} // namespace

int main() {
  try {
    testNavigation();
    testFailureAndLiveUpdates();
    testCurrentFolderFilter();
    std::println("browser controller checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "browser controller test: {}", error.what());
    return 1;
  }
}
