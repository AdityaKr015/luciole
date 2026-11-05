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
    void drainFor(std::chrono::milliseconds duration) {
      const auto deadline = std::chrono::steady_clock::now() + duration;
      until([&] { return std::chrono::steady_clock::now() >= deadline; });
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

  void testActionContextsAndFileLinks() {
    Fixture fixture;
    Pump pump;
    BrowserController browser({}, false);
    browser.open(Location::fromPath(fixture.path.native()));
    pump.ready(browser);
    const auto original = browser.contextSerial();
    browser.refresh();
    pump.ready(browser);
    require(browser.contextSerial() == original, "refresh invalidated the browsing action context");
    browser.setSearchQuery("file");
    pump.ready(browser);
    const auto search = browser.contextSerial();
    require(search != original, "search reused the browsing action context");
    browser.setSearchQuery({});
    pump.ready(browser);
    require(
        browser.contextSerial() != original && browser.contextSerial() != search,
        "returning from search revived an old context"
    );
    const auto beforeChild = browser.contextSerial();
    browser.activate(id(browser, "child"));
    pump.ready(browser);
    browser.back();
    pump.ready(browser);
    require(browser.contextSerial() != beforeChild, "history return revived an old menu context");
    fs::create_symlink("file", fixture.path / "file-link");
    browser.refresh();
    pump.ready(browser);
    std::optional<Location> launched;
    browser.setFileActivatedCallback([&](const Location& file) { launched = file; });
    browser.activate(id(browser, "file-link"));
    pump.until([&] { return launched.has_value(); });
    require(
        launched == Location::fromPath((fixture.path / "file-link").native())
            && browser.model().snapshot().location == Location::fromPath(fixture.path.native()),
        "regular-file symlink activation tried to enumerate a directory"
    );
    launched.reset();
    browser.activate(id(browser, "file-link"));
    browser.open(Location::fromPath((fixture.path / "child").native()));
    pump.ready(browser);
    require(!launched, "a late link result opened after navigation");
    auto retiring = std::make_unique<BrowserController>(files::SortOptions{}, false);
    retiring->open(Location::fromPath(fixture.path.native()));
    pump.ready(*retiring);
    retiring->activate(id(*retiring, "file-link"));
    retiring.reset();
  }

  void testDuplicatedTabs() {
    Fixture fixture;
    Pump pump;
    BrowserController original({}, false), duplicate({}, true);
    const auto root = Location::fromPath(fixture.path.native());
    original.open(root);
    pump.ready(original);
    original.open(Location::fromPath((fixture.path / "child").native()));
    pump.ready(original);
    original.model().select(id(original, "inside"));
    original.setCaptureViewport([] {
      return files::BrowseViewport{.mode = files::ViewMode::LIST, .iconSize = 96, .vertical = 125};
    });
    duplicate.open(Location::fromPath((fixture.path / "other").native()));
    duplicate.duplicateFrom(original);
    pump.ready(duplicate);
    require(
        duplicate.canBack()
            && duplicate.model().selectedIds().size() == 1
            && duplicate.model().entry(duplicate.model().selectedIds().front())->name == "inside",
        "duplicate lost history or stable selection"
    );
    const auto viewport = duplicate.takeRestoredViewport();
    require(
        viewport && viewport->mode == files::ViewMode::LIST && viewport->vertical == 125, "duplicate lost viewport"
    );
    duplicate.back();
    pump.ready(duplicate);
    require(
        original.model().snapshot().location == Location::fromPath((fixture.path / "child").native()),
        "duplicate navigation changed source tab"
    );
    original.setSearchQuery("inside");
    pump.ready(original);
    duplicate.duplicateFrom(original);
    pump.ready(duplicate);
    original.open(root);
    pump.ready(original);
    require(
        duplicate.searchQuery() == "inside" && duplicate.model().rows().size() == 1,
        "source navigation leaked into duplicated search"
    );
    auto closing = std::make_unique<BrowserController>(files::SortOptions{}, false);
    closing->duplicateFrom(duplicate);
    closing.reset();
    pump.drainFor(30ms);
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
    pump.until([&] {
      return browser.model().snapshot().location == Location::fromPath((fixture.path / "folder-link").native());
    });
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
      if (entry.type == G_FILE_TYPE_REGULAR && !entry.symlink) {
        require(entry.contentType == "text/plain", "recursive results lost fast content-type metadata");
      }
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
    require(
        browser.searchQuery() == "target" && browser.model().snapshot().location == root,
        "history did not restore the search visit"
    );

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

  void testHistoryState() {
    Fixture fixture;
    Pump pump;
    BrowserController browser({}, false);
    files::BrowseViewport viewport{
        .mode = files::ViewMode::LIST, .iconSize = 96, .vertical = 123.5F, .horizontal = 42.0F, .withinRow = 9.5F
    };
    browser.setViewDefaults(files::ViewMode::GRID, 48);
    browser.setCaptureViewport([&] { return viewport; });
    browser.open(Location::fromPath(fixture.path.native()));
    pump.ready(browser);
    (void)browser.takeRestoredViewport();
    const auto selected = id(browser, "file");
    const auto focus = id(browser, "child");
    const auto anchor = id(browser, "other");
    browser.model().restoreInteraction({{selected}, focus, anchor}, selected);
    viewport.first = selected;
    browser.open(Location::fromPath((fixture.path / "child").native()));
    pump.ready(browser);
    require(
        browser.viewport().mode == files::ViewMode::GRID && browser.viewport().iconSize == 48,
        "visit view leaked into global defaults"
    );
    require(browser.back(), "history return unavailable");
    pump.ready(browser);
    require(
        browser.model().selectedIds() == std::vector{id(browser, "file")} && id(browser, "file") != selected,
        "history reused an obsolete row ID or lost surviving selection"
    );
    require(
        browser.model().selection().focused == id(browser, "child")
            && browser.model().selection().anchor == id(browser, "other"),
        "history conflated focus, range anchor and selection"
    );
    auto restored = browser.takeRestoredViewport();
    require(
        restored
            && restored->mode == files::ViewMode::LIST
            && restored->iconSize == 96
            && restored->first == id(browser, "file")
            && restored->withinRow == 9.5F
            && restored->horizontal == 42.0F
            && restored->vertical == 123.5F,
        "history lost viewport geometry"
    );
    require(!browser.takeRestoredViewport(), "viewport restoration was not one-shot");
    browser.model().setSort({.field = files::SortField::SIZE, .descending = true, .directoriesFirst = false});
    browser.toggleHidden();
    browser.forward();
    pump.ready(browser);
    // Preserve the old object under another name so inode reuse cannot weaken the replacement check.
    fs::rename(fixture.path / "file", fixture.path / "old-file");
    std::ofstream(fixture.path / "file") << "replacement";
    browser.back();
    pump.ready(browser);
    require(browser.model().selectedIds().empty(), "a replacement at the same path inherited history selection");
    require(
        browser.model().sort().field == files::SortField::SIZE && browser.model().filter().showHidden,
        "history restored obsolete global sort or hidden preferences"
    );
    browser.setSearchQuery("inside");
    pump.ready(browser);
    browser.model().select(id(browser, "inside"));
    viewport.first = id(browser, "inside");
    browser.open(Location::fromPath((fixture.path / "other").native()));
    pump.ready(browser);
    browser.back();
    pump.ready(browser);
    require(
        browser.searchQuery() == "inside" && browser.model().selectedIds() == std::vector{id(browser, "inside")},
        "history did not restore a search visit"
    );
    browser.forward();
    browser.back();
    browser.open(Location::fromPath((fixture.path / "child").native()));
    pump.ready(browser);
    require(
        browser.searchQuery().empty() && browser.model().selectedIds().empty() && !browser.canForward(),
        "late history results reached a new visit"
    );
  }

  void testDebouncedSearch() {
    Fixture fixture;
    Pump pump;
    BrowserController browser({}, false);
    const auto root = Location::fromPath(fixture.path.native());
    const auto child = Location::fromPath((fixture.path / "child").native());
    browser.open(root);
    pump.ready(browser);
    browser.model().select(id(browser, "file"));
    browser.editSearchQuery("file");
    require(
        browser.searchPending()
            && browser.searchQuery() == "file"
            && browser.model().snapshot().status == DirectoryStatus::LOADING
            && browser.model().rows().empty()
            && !browser.model().selectionAvailable()
            && browser.model().selectedIds().empty(),
        "pending query retained actionable browse results or started synchronously"
    );
    browser.editSearchQuery("ins");
    pump.drainFor(80ms);
    browser.editSearchQuery("inside");
    const auto edited = std::chrono::steady_clock::now();
    pump.ready(browser);
    require(std::chrono::steady_clock::now() - edited >= 160ms, "search ignored the trailing debounce");
    require(
        !browser.searchPending()
            && browser.model().rows().size() == 1
            && browser.model().snapshot().entries.front().name == "inside",
        "trailing query did not traverse descendants or accepted an obsolete query"
    );
    browser.editSearchQuery("file");
    browser.submitSearchQuery();
    require(!browser.searchPending(), "Enter left a query waiting for the timer");
    pump.ready(browser);
    require(browser.model().rows().size() == 1 && id(browser, "file"), "submitted query returned wrong results");
    browser.editSearchQuery("inside");
    browser.editSearchQuery({});
    require(
        !browser.searchPending()
            && browser.searchQuery().empty()
            && browser.model().snapshot().status == DirectoryStatus::READY
            && browser.model().selectionAvailable()
            && browser.model().rows().size() == 4,
        "clearing a pending query did not restore browsing immediately"
    );
    pump.drainFor(230ms);
    require(browser.searchQuery().empty() && browser.model().rows().size() == 4, "cleared timer repopulated search");
    browser.editSearchQuery("inside");
    browser.stop();
    pump.drainFor(230ms);
    require(
        !browser.searchPending()
            && browser.model().snapshot().status == DirectoryStatus::CANCELLED
            && browser.model().rows().empty(),
        "stopping a pending query revived old results"
    );
    browser.refresh();
    pump.ready(browser);
    require(browser.model().rows().size() == 1 && id(browser, "inside"), "refresh did not restart a stopped query");
    browser.editSearchQuery("hidden");
    browser.toggleHidden();
    pump.ready(browser);
    require(!browser.searchPending() && id(browser, ".hidden"), "hidden policy did not replace the pending traversal");
    browser.editSearchQuery("inside");
    browser.open(child);
    pump.ready(browser);
    pump.drainFor(230ms);
    require(
        !browser.searchPending()
            && browser.searchQuery().empty()
            && browser.model().snapshot().location == child
            && browser.model().rows().size() == 1,
        "pending query survived navigation"
    );
    browser.back();
    pump.ready(browser);
    require(browser.searchQuery() == "inside" && id(browser, "inside"), "history lost the latest pending query");
    browser.editSearchQuery("file");
    browser.forward();
    pump.ready(browser);
    pump.drainFor(230ms);
    require(
        browser.searchQuery().empty() && browser.model().snapshot().location == child, "history retained an old timer"
    );
    browser.editSearchQuery("inside");
    browser.open(child);
    pump.ready(browser);
    pump.drainFor(230ms);
    require(
        !browser.searchPending() && browser.searchQuery().empty(), "same-folder navigation did not cancel debounce"
    );
  }

  void testSearchCompositionAndTimeoutLifetime() {
    Fixture fixture;
    Pump pump;
    const auto root = Location::fromPath(fixture.path.native());
    auto browser = std::make_unique<BrowserController>(files::SortOptions{}, false);
    browser->open(root);
    pump.ready(*browser);
    browser->editSearchQuery("in");
    browser->setSearchComposing(true);
    browser->submitSearchQuery();
    pump.drainFor(230ms);
    require(
        browser->searchPending() && browser->model().rows().empty(),
        "preedit or Enter during composition launched a query"
    );
    browser->editSearchQuery("inside");
    pump.drainFor(230ms);
    require(browser->searchPending(), "committed changes launched while composition remained active");
    browser->setSearchComposing(false);
    const auto committed = std::chrono::steady_clock::now();
    pump.ready(*browser);
    require(
        std::chrono::steady_clock::now() - committed >= 160ms && id(*browser, "inside"),
        "composition completion did not resume the trailing query"
    );
    browser->editSearchQuery("file");
    browser->setSearchComposing(true);
    browser->editSearchQuery({});
    browser->setSearchComposing(false);
    pump.drainFor(230ms);
    require(
        !browser->searchPending() && browser->model().rows().size() == 4, "cleared composition revived a pending query"
    );
    browser->editSearchQuery("inside");
    browser.reset();
    pump.drainFor(230ms);
    browser = std::make_unique<BrowserController>(files::SortOptions{}, false);
    browser->open(root);
    pump.ready(*browser);
    browser->editSearchQuery("inside");
    browser->setChangedCallback([&] {
      if (!browser->searchPending()) {
        browser.reset();
      }
    });
    pump.until([&] { return !browser; });
    pump.drainFor(230ms);
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
    testActionContextsAndFileLinks();
    testDuplicatedTabs();
    testNavigation();
    testFailureAndLiveUpdates();
    testRecursiveSearch();
    testSearchLifetimeAndPartialFailure();
    testLocationPresentation();
    testHistoryState();
    testDebouncedSearch();
    testSearchCompositionAndTimeoutLifetime();
    std::println("browser controller checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "browser controller test: {}", error.what());
    return 1;
  }
}
