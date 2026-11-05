#include "app/glib_poll_source.h"
#include "browser/miller_columns.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using files::BrowserController;
  using files::DirectoryStatus;
  using files::Location;
  using files::MillerColumns;

  void require(bool value, const std::string& message) {
    if (!value) {
      throw std::runtime_error(message);
    }
  }
  class Fixture {
  public:
    Fixture() {
      const files::GString name(g_dir_make_tmp("noctalia-miller-XXXXXX", nullptr));
      require(name != nullptr, "could not create fixture");
      path = name.get();
      fs::create_directories(path / "a" / "nested");
      fs::create_directory(path / "b");
      std::ofstream(path / "a" / "file.txt") << "column fixture";
      std::ofstream(path / "a" / "nested" / "kept.txt") << "selected fixture";
      fs::create_directory_symlink("..", path / "a" / "cycle");
    }
    ~Fixture() {
      std::error_code ignored;
      fs::permissions(path / "locked", fs::perms::owner_all, ignored);
      fs::remove_all(path, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    fs::path path;
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
      while (!done()) {
        require(std::chrono::steady_clock::now() < deadline, "column fixture timed out");
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        require(::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) >= 0, "poll failed");
        m_source.dispatch(fds, start);
      }
    }
    void ready(const BrowserController& browser) {
      until([&] { return browser.model().snapshot().status != DirectoryStatus::LOADING; });
      require(browser.model().snapshot().status == DirectoryStatus::READY, "column could not load");
    }
    void loaded(MillerColumns& columns) {
      const auto count = columns.columns().size();
      for (auto i = count > 3 ? count - 3 : 0; i < count; ++i) {
        columns.load(i);
      }
      until([&] {
        columns.synchronize();
        return std::ranges::all_of(columns.columns(), [](const auto& column) {
          return column->browser->model().snapshot().status != DirectoryStatus::LOADING;
        });
      });
      columns.synchronize();
    }

  private:
    GlibPollSource m_source;
  };
  files::EntryId entry(const BrowserController& browser, const std::string& name) {
    for (const auto& item : browser.model().snapshot().entries) {
      if (item.name == name) {
        return item.id;
      }
    }
    throw std::runtime_error("missing entry: " + name);
  }

  std::size_t columnAt(const MillerColumns& columns, const fs::path& path) {
    const auto location = Location::fromPath(path.native());
    for (std::size_t i = 0; i < columns.columns().size(); ++i) {
      if (columns.columns()[i]->location == location) {
        return i;
      }
    }
    throw std::runtime_error("missing ancestor: " + path.native());
  }

  void parentChainAndHistory() {
    Fixture fixture;
    Pump pump;
    BrowserController source({}, false);
    const auto current = Location::fromPath((fixture.path / "a" / "nested").native());
    source.open(Location::fromPath(fixture.path.native()));
    pump.ready(source);
    source.open(current);
    pump.ready(source);
    source.model().select(entry(source, "kept.txt"));
    source.setCaptureViewport([] {
      return files::BrowseViewport{.mode = files::ViewMode::LIST, .iconSize = 160, .vertical = 45};
    });
    MillerColumns columns;
    columns.start(source);
    require(
        columns.columns().front()->browser->model().snapshot().status == DirectoryStatus::IDLE,
        "offscreen ancestors started filesystem work before being revealed"
    );
    pump.loaded(columns);
    require(
        columns.focused() + 1 == columns.columns().size() && columns.active().model().snapshot().location == current,
        "parents stole focus or changed the current folder"
    );
    require(columns.columns().front()->location == Location::fromPath("/"), "root ancestor missing");
    require(
        columns.active().model().selection().selected.contains(entry(columns.active(), "kept.txt")),
        "ancestry lost the current selection"
    );
    const auto viewport = columns.active().takeRestoredViewport();
    require(viewport && viewport->vertical == 45 && viewport->iconSize == 160, "ancestry lost the current viewport");
    const auto parentIndex = columnAt(columns, fixture.path / "a");
    const auto& parent = *columns.columns()[parentIndex]->browser;
    require(parent.model().selection().selected.contains(entry(parent, "nested")), "path row was not selected");
    require(!parent.canBack(), "synthetic ancestors polluted browsing history");
    require(columns.active().back(), "real back history was lost");
    pump.loaded(columns);
    require(
        columns.active().model().snapshot().location == Location::fromPath(fixture.path.native()),
        "back did not retain the ancestor target"
    );
    require(columns.active().forward(), "ancestor history discarded forward");
    pump.loaded(columns);
    require(columns.active().model().snapshot().location == current, "forward returned to the wrong location");
    columns.active().open(Location::fromPath((fixture.path / "b").native()));
    pump.loaded(columns);
    require(
        columns.columns().front()->location == Location::fromPath("/")
            && columns.active().model().snapshot().location == Location::fromPath((fixture.path / "b").native()),
        "a new location did not rebuild ancestry"
    );
  }

  void hiddenLinksAndFailedParents() {
    Fixture fixture;
    Pump pump;
    fs::create_directories(fixture.path / ".hidden" / "leaf");
    fs::create_directory_symlink("a", fixture.path / "link");
    BrowserController source({}, false);
    source.open(Location::fromPath((fixture.path / ".hidden" / "leaf").native()));
    pump.ready(source);
    MillerColumns columns;
    columns.start(source);
    pump.loaded(columns);
    require(
        columns.active().model().snapshot().location == source.model().snapshot().location,
        "a hidden ancestor discarded the current folder"
    );
    const auto hiddenParent = columnAt(columns, fixture.path);
    require(
        columns.columns()[hiddenParent]->browser->model().selection().selected.empty(),
        "hidden path selected an invisible row"
    );
    columns.focus(hiddenParent);
    columns.interact(hiddenParent);
    columns.synchronize();
    require(columns.columns().size() == hiddenParent + 1, "explicit deselection revived a hidden path");
    source.open(Location::fromPath((fixture.path / "link" / "nested").native()));
    pump.ready(source);
    columns.start(source);
    pump.loaded(columns);
    require(
        columns.active().model().snapshot().location == source.model().snapshot().location,
        "an explicit directory link discarded descendants"
    );
    const auto linkParent = columnAt(columns, fixture.path);
    const auto& parent = *columns.columns()[linkParent]->browser;
    require(parent.model().selection().selected.contains(entry(parent, "link")), "linked path was not highlighted");
    source.open(Location::fromPath((fixture.path / "missing" / "leaf").native()));
    pump.until([&] { return source.model().snapshot().status == DirectoryStatus::ERROR; });
    columns.start(source);
    pump.loaded(columns);
    require(
        columns.active().model().snapshot().status == DirectoryStatus::ERROR
            && columns.active().model().snapshot().location == source.model().snapshot().location,
        "missing ancestry replaced the requested error view"
    );
    if (::getuid() != 0) {
      fs::create_directories(fixture.path / "locked" / "leaf");
      fs::permissions(fixture.path / "locked", fs::perms::owner_exec);
      source.open(Location::fromPath((fixture.path / "locked" / "leaf").native()));
      pump.ready(source);
      columns.start(source);
      pump.loaded(columns);
      require(
          columns.active().model().snapshot().status == DirectoryStatus::READY
              && columns.columns()[columnAt(columns, fixture.path / "locked")]->browser->model().snapshot().status
                  == DirectoryStatus::ERROR,
          "unreadable parent prevented browsing the accessible folder"
      );
    }
  }

  void boundedEntryAndEarlyInteraction() {
    Fixture fixture;
    auto path = fixture.path;
    for (std::size_t i = 0; i < MillerColumns::maxColumns + 2; ++i) {
      path /= "deep";
      fs::create_directory(path);
    }
    Pump pump;
    BrowserController source({}, false);
    source.open(Location::fromPath(path.native()));
    pump.ready(source);
    MillerColumns columns;
    columns.start(source);
    require(columns.columns().size() == MillerColumns::maxColumns, "entry exceeded the ancestor session bound");
    pump.loaded(columns);
    require(
        columns.active().model().snapshot().location == source.model().snapshot().location,
        "deep ancestry lost the requested folder"
    );
    source.open(Location::fromPath((fixture.path / "a" / "nested").native()));
    pump.ready(source);
    columns.start(source);
    const auto index = columnAt(columns, fixture.path);
    columns.load(index);
    pump.ready(*columns.columns()[index]->browser);
    columns.focus(index);
    columns.interact(index);
    auto& parent = *columns.columns()[index]->browser;
    parent.model().select(entry(parent, "b"));
    pump.loaded(columns);
    require(
        columns.columns().back()->location == Location::fromPath((fixture.path / "b").native()),
        "late ancestor seeding overwrote the user's branch"
    );
    columns.start(source, false);
    pump.loaded(columns);
    require(
        columns.columns().size() == 1
            && columns.active().model().snapshot().location == source.model().snapshot().location,
        "disabling parents did not restore a single starting column"
    );
  }

  void branchesAndSelection() {
    Fixture fixture;
    Pump pump;
    BrowserController source({}, false);
    source.open(Location::fromPath(fixture.path.native()));
    pump.ready(source);
    source.setCaptureViewport([] { return files::BrowseViewport{.mode = files::ViewMode::MILLER, .iconSize = 192}; });
    MillerColumns columns;
    columns.start(source, false);
    pump.ready(columns.active());
    require(columns.active().contextSerial() != source.contextSerial(), "column context aliases its source");
    auto& parent = columns.active();
    parent.model().select(entry(parent, "a"));
    columns.synchronize();
    require(columns.columns().size() == 2 && columns.focused() == 0, "selection should reveal without stealing focus");
    require(columns.enter(), "right/enter did not focus the child");
    pump.ready(columns.active());
    require(
        columns.active().model().snapshot().location == Location::fromPath((fixture.path / "a").native()), "wrong child"
    );
    require(columns.active().viewport().iconSize == 192, "column navigation lost the saved grid zoom");
    columns.columns()[0]->width = 345;
    columns.active().model().select(entry(columns.active(), "nested"));
    columns.synchronize();
    require(columns.columns().size() == 3, "nested folder did not expand");
    auto* old = columns.columns()[2]->browser.get();
    require(columns.focus(0), "could not return to parent");
    parent.model().select(entry(parent, "b"));
    columns.synchronize();
    require(columns.columns().size() == 2, "sibling change retained descendants");
    require(
        columns.columns()[1]->location == Location::fromPath((fixture.path / "b").native()), "wrong sibling target"
    );
    require(columns.columns()[0]->width == 345, "sibling change lost ancestor width");
    require(old->model().snapshot().status == DirectoryStatus::CANCELLED, "retired column is still loading");
    parent.model().select(entry(parent, "a"), files::SelectionMode::TOGGLE);
    columns.synchronize();
    require(columns.columns().size() == 1, "multiple folders opened competing branches");
    columns.collectRetired();
    parent.model().clearSelection();
    columns.synchronize();
    require(columns.columns().size() == 1 && !columns.enter(), "empty selection entered stale child");
  }

  void historySearchAndReplacement() {
    Fixture fixture;
    Pump pump;
    BrowserController source({}, false);
    source.open(Location::fromPath(fixture.path.native()));
    pump.ready(source);
    MillerColumns columns;
    columns.start(source, false);
    pump.ready(columns.active());
    columns.active().model().select(entry(columns.active(), "a"));
    require(columns.enter(), "could not enter a");
    pump.ready(columns.active());
    const auto context = columns.active().contextSerial();
    columns.active().model().select(entry(columns.active(), "cycle"));
    columns.synchronize();
    require(columns.columns().size() == 2, "symlink recursively expanded a cycle");
    columns.active().model().select(entry(columns.active(), "nested"));
    columns.synchronize();
    require(columns.columns().size() == 3, "missing nested branch");
    columns.active().setSearchQuery("file");
    columns.synchronize();
    require(columns.columns().size() == 2, "search retained an unrelated child");
    pump.ready(columns.active());
    columns.active().setSearchQuery("");
    require(columns.active().back(), "child has no inherited history");
    columns.synchronize();
    pump.ready(columns.active());
    require(
        columns.focused() == 0 && columns.active().model().snapshot().location == source.model().snapshot().location,
        "back did not restore parent"
    );
    require(columns.active().contextSerial() != context, "history revived stale operation targets");
    require(columns.active().forward(), "back discarded forward history");
    columns.synchronize();
    pump.ready(columns.active());
    require(
        columns.active().model().snapshot().location == Location::fromPath((fixture.path / "a").native()),
        "forward has wrong location"
    );
    columns.active().model().select(entry(columns.active(), "nested"));
    columns.synchronize();
    require(columns.columns().size() == 2, "forward branch missing");
    const auto oldId = columns.columns().back()->id;
    fs::rename(fixture.path / "a" / "nested", fixture.path / "a" / "retired");
    fs::create_directory(fixture.path / "a" / "nested");
    columns.active().refresh();
    pump.ready(columns.active());
    columns.synchronize();
    require(
        columns.columns().size() == 1 || columns.columns().back()->id != oldId, "replacement retained stale descendant"
    );
    columns.clear();
    columns.collectRetired();
  }

  void resourceBoundAndIndependentPanes() {
    Fixture fixture;
    auto path = fixture.path;
    for (std::size_t i = 0; i < MillerColumns::maxColumns + 2; ++i) {
      path /= "deep";
      fs::create_directory(path);
    }
    Pump pump;
    BrowserController source({}, false);
    source.open(Location::fromPath(fixture.path.native()));
    pump.ready(source);
    MillerColumns first, other;
    first.start(source, false);
    other.start(source, false);
    pump.ready(other.active());
    const auto otherContext = other.active().contextSerial();
    for (std::size_t i = 0; i < MillerColumns::maxColumns + 2; ++i) {
      pump.ready(first.active());
      first.active().model().select(entry(first.active(), "deep"));
      require(first.enter(), "column bound prevented deeper navigation");
      require(first.columns().size() <= MillerColumns::maxColumns, "column resource bound failed");
    }
    require(first.columns().size() == MillerColumns::maxColumns, "unbounded column sessions");
    require(
        other.columns().size() == 1
            && other.active().contextSerial() == otherContext
            && other.active().model().selection().selected.empty(),
        "independent pane was modified"
    );
    first.clear();
    first.collectRetired();
  }
} // namespace

int main() {
  try {
    branchesAndSelection();
    parentChainAndHistory();
    hiddenLinksAndFailedParents();
    boundedEntryAndEarlyInteraction();
    historySearchAndReplacement();
    resourceBoundAndIndependentPanes();
    std::println("Miller column checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "{}", error.what());
    return 1;
  }
}
