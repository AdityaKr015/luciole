#include "app/glib_poll_source.h"
#include "browser/details_columns.h"
#include "browser/directory_model.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <limits>
#include <print>
#include <stdexcept>

namespace {
  using namespace std::chrono_literals;
  using files::DirectoryEntry;
  using files::DirectoryModel;
  using files::DirectorySnapshot;
  using files::DirectoryStatus;
  using files::EntryId;
  using files::Location;
  using files::SelectionMode;
  using files::SortField;

  void require(bool condition, const std::string& message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  DirectoryEntry entry(EntryId id, const std::string& name, GFileType type = G_FILE_TYPE_REGULAR) {
    return {
        .id = id,
        .location = Location::fromPath("/tmp/noctalia-model-fixture/" + name),
        .name = name,
        .displayName = name,
        .type = type,
        .size = std::nullopt,
        .modifiedSeconds = std::nullopt,
        .etag = std::nullopt,
        .parentDisplayName = {}
    };
  }

  DirectorySnapshot snapshot(std::vector<DirectoryEntry> entries, std::uint64_t generation = 1) {
    DirectorySnapshot result;
    result.generation = generation;
    result.location = Location::fromPath("/tmp/noctalia-model-fixture");
    result.status = DirectoryStatus::READY;
    result.entries = std::move(entries);
    return result;
  }

  void rows(const DirectoryModel& model, const std::vector<EntryId>& expected) {
    require(model.rows() == expected, "unexpected visible row order");
  }

  void selected(const DirectoryModel& model, const std::vector<EntryId>& expected) {
    require(model.selectedIds() == expected, "unexpected selected IDs or action order");
  }

  void testSortAndMetadata() {
    DirectoryModel model;
    auto data = snapshot(
        {entry(10, "item10"), entry(2, "item2"), entry(1, "item1"), entry(30, "z-folder", G_FILE_TYPE_DIRECTORY),
         entry(20, "a-folder", G_FILE_TYPE_DIRECTORY)}
    );
    model.applySnapshot(data);
    rows(model, {20, 30, 1, 2, 10});
    model.setSort({.descending = true});
    rows(model, {30, 20, 10, 2, 1});
    model.setSort({.directoriesFirst = false});
    rows(model, {20, 1, 2, 10, 30});

    data.entries = {entry(1, "a"), entry(2, "b"), entry(3, "c")};
    data.entries[0].size = 0;
    data.entries[0].modifiedSeconds = 0;
    data.entries[1].size = 5;
    data.entries[1].modifiedSeconds = 20;
    model.applySnapshot(data);
    for (const auto field : {SortField::SIZE, SortField::MODIFIED}) {
      model.setSort({.field = field});
      rows(model, {1, 2, 3});
      model.setSort({.field = field, .descending = true});
      rows(model, {2, 1, 3});
    }

    // Collation ties must not let enumeration order determine action targets.
    data.entries = {entry(5, "same-b"), entry(4, "same-a")};
    for (auto& item : data.entries) {
      item.displayName = "Same";
    }
    model.setSort({});
    model.applySnapshot(data);
    const auto order = model.rows();
    std::ranges::reverse(data.entries);
    model.applySnapshot(data);
    require(model.rows() == order, "collation ties changed after enumeration reorder");
    require(model.entry(4)->name == "same-a", "sorting changed the operation name");
  }

  void testDetailsLayout() {
    const auto wide = files::DetailsColumns::forWidth(1000);
    require(
        wide.contentWidth == 1000 && wide.nameWidth == 558 && wide.showType,
        "automatic Name failed to fill the remaining viewport"
    );
    const auto ultrawide = files::DetailsColumns::forWidth(4000);
    require(
        ultrawide.nameWidth == 3558 && ultrawide.contentWidth == 4000,
        "automatic Name did not fill an ultrawide viewport"
    );
    const auto bounded = files::DetailsColumns::forWidth(100000);
    require(
        files::validDetailsColumnWidth(0, static_cast<int>(bounded.nameWidth)),
        "automatic Name could not be frozen into a valid width preference"
    );
    const files::DetailsColumnWidths requested{400, 200, 96, 180};
    const auto narrow = files::DetailsColumns::forWidth(300, {}, requested);
    const auto expanded = files::DetailsColumns::forWidth(1600, {}, requested);
    require(
        narrow == expanded && narrow.contentWidth == 892 && narrow.showType && narrow.showSize && narrow.showModified,
        "temporary constraints changed explicit column requests or hid a requested column"
    );
    const auto hidden = files::DetailsColumns::forWidth(300, {false, false, false}, requested);
    require(hidden.contentWidth == 416 && hidden.nameWidth == 400, "hidden metadata left a column gap");
    const auto automatic = files::DetailsColumns::forWidth(300);
    require(
        automatic.nameWidth == 160 && automatic.contentWidth == 602 && automatic.showType,
        "narrow automatic layout failed to expose horizontal overflow"
    );
  }

  void testDetailsColumnMoves() {
    using Column = files::DetailsColumn;
    const auto defaults = files::kDefaultDetailsColumnOrder;
    require(
        !files::moveDetailsColumn(defaults, Column::TYPE, -1, {})
            && !files::moveDetailsColumn(defaults, Column::MODIFIED, 1, {}),
        "column movement crossed a protected end"
    );
    require(
        files::moveDetailsColumn(defaults, Column::TYPE, 1, {})
            == files::DetailsColumnOrder{Column::SIZE, Column::TYPE, Column::MODIFIED},
        "column did not move to its next visible neighbor"
    );
    require(
        files::moveDetailsColumn(defaults, Column::MODIFIED, -1, {})
            == files::DetailsColumnOrder{Column::TYPE, Column::MODIFIED, Column::SIZE},
        "column did not move to its previous visible neighbor"
    );
    require(
        files::moveDetailsColumn(defaults, Column::TYPE, 1, {.size = false})
            == files::DetailsColumnOrder{Column::MODIFIED, Column::SIZE, Column::TYPE},
        "hidden neighbor consumed a visible move"
    );
    require(
        !files::moveDetailsColumn(defaults, Column::TYPE, -1, {.type = false})
            && !files::moveDetailsColumn(defaults, Column::TYPE, 1, {.size = false, .modified = false})
            && !files::moveDetailsColumn(defaults, Column::TYPE, 0, {})
            && !files::moveDetailsColumn({Column::TYPE, Column::TYPE, Column::SIZE}, Column::TYPE, 1, {}),
        "hidden, solitary or invalid column movement changed order"
    );
  }

  void testDetailsColumnInsertion() {
    using Column = files::DetailsColumn;
    const auto defaults = files::kDefaultDetailsColumnOrder;
    require(
        files::insertDetailsColumnBefore(defaults, Column::TYPE, std::nullopt)
            == files::DetailsColumnOrder{Column::SIZE, Column::MODIFIED, Column::TYPE},
        "column insertion did not append after the final target"
    );
    require(
        files::insertDetailsColumnBefore(defaults, Column::MODIFIED, Column::TYPE)
            == files::DetailsColumnOrder{Column::MODIFIED, Column::TYPE, Column::SIZE},
        "column insertion did not preserve the other columns' relative order"
    );
    require(
        files::insertDetailsColumnBefore(defaults, Column::TYPE, Column::TYPE) == defaults
            && files::insertDetailsColumnBefore(defaults, Column::SIZE, Column::MODIFIED) == defaults,
        "self or adjacent insertion changed the order"
    );
    require(
        !files::insertDetailsColumnBefore(defaults, static_cast<Column>(99), std::nullopt)
            && !files::insertDetailsColumnBefore(defaults, Column::TYPE, static_cast<Column>(99))
            && !files::insertDetailsColumnBefore(
                {Column::TYPE, Column::TYPE, Column::SIZE}, Column::TYPE, std::nullopt
            ),
        "invalid column insertion was accepted"
    );
  }

  void testTypeSort() {
    DirectoryModel model;
    auto data = snapshot(
        {entry(10, "z-folder", G_FILE_TYPE_DIRECTORY), entry(2, "link"), entry(3, "note10.txt"), entry(4, "note2.txt"),
         entry(5, "z.png"), entry(6, "a.bin"), entry(7, "b"), entry(8, "special", G_FILE_TYPE_SPECIAL),
         entry(9, "c", G_FILE_TYPE_UNKNOWN)}
    );
    data.entries[1].symlink = true;
    data.entries[1].contentType = "image/png";
    data.entries[2].contentType = "text/plain";
    data.entries[3].contentType = "text/plain";
    data.entries[4].contentType = "image/png";
    data.entries[5].contentType = "application/octet-stream";
    model.applySnapshot(data);
    model.select(3);
    model.select(5, SelectionMode::TOGGLE);
    model.setScrollAnchor(3);
    model.setSort({.field = SortField::TYPE, .directoriesFirst = false});
    rows(model, {10, 2, 4, 3, 5, 8, 6, 7, 9});
    require(
        model.typeLabel(10) == "Folder"
            && model.typeLabel(2) == "Link"
            && model.typeLabel(3) == model.typeLabel(4)
            && model.typeLabel(3) != model.typeLabel(5)
            && model.typeLabel(6) == "File"
            && model.typeLabel(9) == "Unknown",
        "type labels lost MIME distinctions or link/unknown classification"
    );
    model.setSort({.field = SortField::TYPE, .descending = true, .directoriesFirst = false});
    rows(model, {8, 5, 3, 4, 2, 10, 9, 7, 6});
    selected(model, {5, 3});
    require(
        model.selection().focused == 5 && model.selection().anchor == 5 && model.scrollAnchor() == 3,
        "type sorting changed stable selection/focus/scroll anchors"
    );
    std::ranges::reverse(data.entries);
    model.applySnapshot(data);
    rows(model, {8, 5, 3, 4, 2, 10, 9, 7, 6});
    model.setSort({.field = SortField::TYPE, .descending = true});
    rows(model, {10, 8, 5, 3, 4, 2, 9, 7, 6});
    for (auto& item : data.entries)
      if (item.id == 3)
        item.contentType = "image/png";
    model.applySnapshot(data);
    require(model.typeLabel(3) == model.typeLabel(5), "metadata update retained an obsolete type label");
    selected(model, {5, 3});
  }

  void testSelectionSummary() {
    DirectoryModel model;
    auto data = snapshot(
        {entry(1, "empty"), entry(2, "known"), entry(3, "unknown"), entry(4, "folder", G_FILE_TYPE_DIRECTORY),
         entry(5, "link"), entry(6, "special", G_FILE_TYPE_SPECIAL), entry(7, ".hidden")}
    );
    data.entries[0].size = 0;
    data.entries[1].size = 5;
    data.entries[3].size = 4096;
    data.entries[4].symlink = true;
    data.entries[4].size = 900;
    data.entries[5].size = 777;
    data.entries[6].hidden = true;
    data.entries[6].size = 12;
    model.applySnapshot(data);
    model.selectAll();
    const files::SelectionSummary expected{.files = 3, .folders = 1, .others = 2, .bytes = 5, .unknownSizes = 1};
    require(
        model.selectionSummary() == expected, "selection summary counted unknown/folder/link sizes as file contents"
    );
    model.setFilter({.showHidden = true, .text = {}});
    model.selectAll();
    require(
        model.selectionSummary().bytes == 17 && model.selectionSummary().files == 4, "hidden selection size missing"
    );
    model.setFilter({});
    require(model.selectionSummary() == expected, "filtered-out selections still contributed sizes");
    data.entries[1].size = 10;
    model.applySnapshot(data);
    require(model.selectionSummary().bytes == 10, "metadata refresh left stale selected bytes");
    require(model.replaceSelection(std::array<EntryId, 2>{1, 2}), "cannot select known-size files");
    require(
        model.selectionSummary() == files::SelectionSummary{.files = 2, .bytes = 10}, "zero-byte file became unknown"
    );
    data.status = DirectoryStatus::CANCELLED;
    model.applySnapshot(data);
    require(model.selectionSummary() == files::SelectionSummary{}, "cancelled selection retained totals");
    auto huge = snapshot({entry(10, "large"), entry(11, "one")}, 2);
    huge.entries[0].size = std::numeric_limits<std::uint64_t>::max();
    huge.entries[1].size = 1;
    model.applySnapshot(huge);
    model.selectAll();
    require(
        model.selectionSummary()
            == files::
                SelectionSummary{.files = 2, .bytes = std::numeric_limits<std::uint64_t>::max(), .overflow = true},
        "selected byte totals wrapped on overflow"
    );
    model.clearSelection();
    require(model.selectionSummary() == files::SelectionSummary{}, "clearing selection retained totals");
  }

  void testFiltering() {
    DirectoryModel model;
    auto data = snapshot({entry(1, "Straße"), entry(2, "café"), entry(3, ".hidden"), entry(4, "*.txt")});
    data.entries[2].hidden = true;
    model.applySnapshot(data);
    require(model.rows().size() == 3, "hidden entries visible by default");
    model.setFilter({.text = "STRASSE"});
    rows(model, {1});
    model.setFilter({.text = "CAFE\u0301"});
    rows(model, {2});
    model.setFilter({.text = "*"});
    rows(model, {4});
    model.setFilter({.showHidden = true, .text = "hidden"});
    rows(model, {3});
    require(model.entry(3)->name == ".hidden", "display filtering rewrote a filename");
    require(model.snapshot().entries.size() == 4, "filter removed source entries");
    model.setFilter({.text = "absent"});
    rows(model, {});
    require(model.snapshot().status == DirectoryStatus::READY, "empty projection changed source status");
  }

  void testSelectionAndAnchors() {
    DirectoryModel model;
    model.applySnapshot(snapshot({entry(1, "a"), entry(2, "b"), entry(3, "c"), entry(4, "d")}));
    model.select(2);
    model.setScrollAnchor(2);
    model.setSort({.descending = true});
    selected(model, {2});
    require(
        model.selection().focused == 2 && model.selection().anchor == 2 && model.scrollAnchor() == 2,
        "reorder moved identity-based anchors"
    );
    model.select(4, SelectionMode::RANGE);
    selected(model, {4, 3, 2});
    require(model.selection().anchor == 2 && model.selection().focused == 4, "range moved its anchor");
    model.select(3, SelectionMode::RANGE);
    selected(model, {3, 2});
    model.select(1, SelectionMode::TOGGLE);
    selected(model, {3, 2, 1});
    model.select(1, SelectionMode::TOGGLE);
    selected(model, {3, 2});
    require(model.selection().anchor == 1 && model.selection().focused == 1, "toggle coupled focus to selection");
    model.select(4, SelectionMode::AddRange);
    selected(model, {4, 3, 2, 1});
    model.clearSelection();
    require(model.selection().focused == 4 && !model.selection().anchor, "clear selection changed focus");
    model.setFocus(3);
    model.select(1, SelectionMode::RANGE);
    selected(model, {3, 2, 1});
    require(model.selection().anchor == 3, "range without an anchor did not start at focus");
    require(!model.select(999) && !model.setFocus(999) && !model.setScrollAnchor(999), "unknown ID accepted");
    selected(model, {3, 2, 1});
    model.setFilter({.text = "b"});
    selected(model, {2});
    require(!model.selection().focused && !model.selection().anchor, "filtered anchors were kept");
    require(model.scrollAnchor() == 2, "visible scroll anchor was cleared");
    require(!model.select(1), "filtered ID selected");
    model.selectAll();
    selected(model, {2});
    model.setFilter({});
    selected(model, {2});
  }

  void testBulkSelection() {
    DirectoryModel model;
    model.applySnapshot(snapshot({entry(1, "a"), entry(2, "b"), entry(3, "c")}));
    int publications = 0;
    model.setChangedCallback([&] { ++publications; });
    const std::array initial{files::EntryId{1}, files::EntryId{3}, files::EntryId{1}};
    require(model.replaceSelection(initial), "bulk selection rejected visible identities");
    selected(model, {1, 3});
    require(
        publications == 1 && model.selection().anchor == 1 && model.selection().focused == 1,
        "bulk selection published intermediate states or lost anchors"
    );
    const std::array invalid{files::EntryId{2}, files::EntryId{999}};
    require(!model.replaceSelection(invalid), "bulk selection accepted an unknown identity");
    selected(model, {1, 3});
    require(publications == 1, "failed bulk selection published a partial replacement");
    const std::array reduced{files::EntryId{3}};
    require(model.replaceSelection(reduced), "shrinking area selection failed");
    selected(model, {3});
    model.setSort({.descending = true});
    require(model.replaceSelection(initial), "reordering invalidated visible identities");
    selected(model, {3, 1});
    model.setFilter({.text = "c"});
    require(!model.replaceSelection(initial), "bulk selection restored a filtered identity");
    selected(model, {3});
    require(model.replaceSelection({}) && !model.selection().anchor, "empty area did not clear selected identities");
    selected(model, {});
    auto stopped = model.snapshot();
    stopped.status = DirectoryStatus::CANCELLED;
    model.applySnapshot(stopped);
    require(!model.replaceSelection(reduced), "bulk selection accepted a cancelled snapshot");
  }

  void testSnapshotReconciliation() {
    DirectoryModel model;
    auto data = snapshot({entry(1, "a"), entry(2, "b")}, 10);
    model.applySnapshot(data);
    model.select(1);
    model.setScrollAnchor(1);
    auto loading = data;
    loading.generation = 11;
    loading.status = DirectoryStatus::LOADING;
    model.applySnapshot(loading);
    selected(model, {1});
    require(!model.applySnapshot(data), "obsolete generation accepted");
    require(model.snapshot().status == DirectoryStatus::LOADING, "stale result replaced loading state");
    data.generation = 11;
    data.entries[0] = entry(1, "z-renamed");
    model.applySnapshot(data);
    selected(model, {1});
    require(model.entry(1)->name == "z-renamed" && model.scrollAnchor() == 1, "rename lost identity");
    data.generation = 12;
    data.entries[0] = entry(3, "z-renamed");
    model.applySnapshot(data);
    selected(model, {});
    require(
        !model.selection().focused && !model.selection().anchor && !model.scrollAnchor(),
        "replacement inherited selection or anchors"
    );
    model.select(2);
    auto cancelled = data;
    cancelled.generation = 13;
    cancelled.status = DirectoryStatus::CANCELLED;
    model.applySnapshot(cancelled);
    selected(model, {});
    require(!model.select(2), "cancelled snapshot accepts selection");
    cancelled.generation = 14;
    cancelled.status = DirectoryStatus::LOADING;
    model.applySnapshot(cancelled);
    require(!model.select(2), "resume selected entries from an unobserved interval");
    cancelled.status = DirectoryStatus::ERROR;
    cancelled.error = files::FileError{.kind = files::FileErrorKind::DENIED, .message = "partial fixture"};
    model.applySnapshot(cancelled);
    require(model.snapshot().error && model.select(2), "partial error lost available entries");
    auto other = snapshot({entry(2, "other")}, 15);
    other.location = Location::fromPath("/tmp/another-model-location");
    model.applySnapshot(other);
    selected(model, {});
    require(!model.selection().focused, "navigation reused selection at an overlapping ID");
    auto dying = std::make_unique<DirectoryModel>();
    dying->setChangedCallback([&dying] { dying.reset(); });
    dying->applySnapshot(other);
  }

  struct Fixture {
    std::filesystem::path path;
    Fixture() {
      const files::GString directory(g_dir_make_tmp("noctalia-model-test-XXXXXX", nullptr));
      require(directory != nullptr, "cannot create private fixture directory");
      path = directory.get();
    }
    ~Fixture() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    void write(const std::string& name) const {
      std::ofstream file(path / name);
      file << "fixture";
      require(file.good(), "cannot write fixture file");
    }
  };

  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!done()) {
        require(std::chrono::steady_clock::now() < deadline, "directory model integration timed out");
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        const int timeout = requested < 0 ? 20 : std::min(requested, 20);
        require(::poll(fds.data(), fds.size(), timeout) >= 0, "poll failed");
        m_source.dispatch(fds, start);
      }
    }

  private:
    GlibPollSource m_source;
  };

  void testDirectoryServiceIntegration() {
    Fixture fixture;
    const std::string name = "item2#%\n" + std::string(1, static_cast<char>(0xFF));
    fixture.write(name);
    fixture.write("item10");
    Pump pump;
    DirectoryModel model;
    files::DirectorySession session;
    session.setChangedCallback([&] { model.applySnapshot(session.snapshot()); });
    session.open(Location::fromPath(fixture.path.native()));
    pump.until([&] { return model.snapshot().status == DirectoryStatus::READY; });
    require(model.rows().size() == 2, "service snapshot not projected");
    const auto id = model.rows().front();
    const auto otherId = model.rows().back();
    require(model.entry(id)->name == name, "native name was altered during projection");
    model.select(id);
    model.setScrollAnchor(id);
    std::filesystem::rename(fixture.path / name, fixture.path / "item20");
    pump.until([&] { return model.entry(id) && model.entry(id)->name == "item20"; });
    rows(model, {otherId, id});
    selected(model, {id});
    require(model.selection().focused == id && model.scrollAnchor() == id, "monitored rename lost anchors");
    session.refresh();
    selected(model, {id});
    pump.until([&] { return model.snapshot().status == DirectoryStatus::READY; });
    selected(model, {id});
    std::filesystem::remove(fixture.path / "item20");
    pump.until([&] { return model.entry(id) == nullptr; });
    selected(model, {});
    fixture.write("item20");
    pump.until([&] { return model.rows().size() == 2; });
    require(model.entry(id) == nullptr && model.selection().selected.empty(), "recreated entry inherited selection");
  }
} // namespace

int main() {
  try {
    // MIME descriptions consult language environment variables independently of collation.
    g_setenv("LC_ALL", "C", true);
    g_setenv("LANGUAGE", "C", true);
    std::setlocale(LC_ALL, "C");
    testSortAndMetadata();
    testDetailsLayout();
    testDetailsColumnMoves();
    testDetailsColumnInsertion();
    testTypeSort();
    testSelectionSummary();
    testFiltering();
    testSelectionAndAnchors();
    testBulkSelection();
    testSnapshotReconciliation();
    testDirectoryServiceIntegration();
    std::println("directory model checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "directory model test: {}", error.what());
    return 1;
  }
}
