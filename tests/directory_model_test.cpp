#include "browser/directory_model.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "model/details_columns.h"
#include "test_check.h"

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

  DirectoryEntry entry(EntryId id, const std::string& name, files::FileType type = files::FileType::Regular) {
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
    result.status = DirectoryStatus::Ready;
    result.entries = std::move(entries);
    return result;
  }

  void rows(const DirectoryModel& model, const std::vector<EntryId>& expected) { TEST_CHECK(model.rows() == expected); }

  void selected(const DirectoryModel& model, const std::vector<EntryId>& expected) {
    TEST_CHECK(model.selectedIds() == expected);
  }

  void testSortAndMetadata() {
    DirectoryModel model;
    auto data = snapshot(
        {entry(10, "item10"), entry(2, "item2"), entry(1, "item1"), entry(30, "z-folder", files::FileType::Directory),
         entry(20, "a-folder", files::FileType::Directory)}
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
    for (const auto field : {SortField::Size, SortField::Modified}) {
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
    TEST_CHECK(model.rows() == order);
    TEST_CHECK(model.entry(4)->name == "same-a");
  }

  void testExtendedMetadataSort() {
    auto a = entry(1, "a"), b = entry(2, "b"), unknown = entry(3, "unknown");
    a.details = {.accessedSeconds = 0, .createdSeconds = 5, .owner = "alpha", .group = "group-a", .mode = 0};
    b.details = {.accessedSeconds = 10, .createdSeconds = 20, .owner = "beta", .group = "group-b", .mode = 0644};
    DirectoryModel model;
    model.applySnapshot(snapshot({unknown, b, a}));
    model.select(1, SelectionMode::Replace);
    for (const auto field :
         {SortField::Accessed, SortField::Created, SortField::Owner, SortField::Group, SortField::Permissions}) {
      model.setSort({field, false, false});
      rows(model, {1, 2, 3});
      model.setSort({field, true, false});
      rows(model, {2, 1, 3});
      TEST_CHECK(model.selection().selected.contains(1));
    }
    a.location = Location::fromPath("/tmp/a/file");
    b.location = Location::fromPath("/tmp/b/file");
    model.applySnapshot(snapshot({b, a}));
    model.setSort({SortField::Location, false, false});
    rows(model, {1, 2});
    model.setSort({SortField::Location, true, false});
    rows(model, {2, 1});
    const auto columns = files::DetailsColumns::forWidth(300, files::allDetailsColumns());
    TEST_CHECK(columns.contentWidth > 1500 && columns.nameWidth == 160);
    for (const auto column : files::kDefaultDetailsColumnOrder) {
      TEST_CHECK(columns.visible(column) && columns.width(column) >= 56);
    }
    const auto reordered = files::insertDetailsColumnBefore(
        files::kDefaultDetailsColumnOrder, files::DetailsColumn::Owner, files::DetailsColumn::Type
    );
    TEST_CHECK(reordered && reordered->front() == files::DetailsColumn::Owner);
  }

  void testDetailsLayout() {
    const auto wide = files::DetailsColumns::forWidth(1000);
    TEST_CHECK(wide.contentWidth == 1000 && wide.nameWidth == 558 && wide.showType);
    const auto ultrawide = files::DetailsColumns::forWidth(4000);
    TEST_CHECK(ultrawide.nameWidth == 3558 && ultrawide.contentWidth == 4000);
    const auto bounded = files::DetailsColumns::forWidth(100000);
    TEST_CHECK(files::validDetailsColumnWidth(0, static_cast<int>(bounded.nameWidth)));
    const files::DetailsColumnWidths requested{400, 200, 96, 180, 156, 156, 128, 128, 112, 260};
    const auto narrow = files::DetailsColumns::forWidth(300, {}, requested);
    const auto expanded = files::DetailsColumns::forWidth(1600, {}, requested);
    TEST_CHECK(
        narrow == expanded && narrow.contentWidth == 892 && narrow.showType && narrow.showSize && narrow.showModified
    );
    const auto hidden = files::DetailsColumns::forWidth(300, {false, false, false}, requested);
    TEST_CHECK(hidden.contentWidth == 416 && hidden.nameWidth == 400);
    const auto automatic = files::DetailsColumns::forWidth(300);
    TEST_CHECK(automatic.nameWidth == 160 && automatic.contentWidth == 602 && automatic.showType);
  }

  void testDetailsColumnMoves() {
    using Column = files::DetailsColumn;
    const auto defaults = files::kDefaultDetailsColumnOrder;
    TEST_CHECK(
        !files::moveDetailsColumn(defaults, Column::Type, -1, {})
        && !files::moveDetailsColumn(defaults, Column::Modified, 1, {})
    );
    TEST_CHECK(
        (files::moveDetailsColumn(defaults, Column::Type, 1, {})
         == files::DetailsColumnOrder{
             Column::Size, Column::Type, Column::Modified, Column::Accessed, Column::Created, Column::Owner,
             Column::Group, Column::Permissions, Column::Location
         })
    );
    TEST_CHECK(
        (files::moveDetailsColumn(defaults, Column::Modified, -1, {})
         == files::DetailsColumnOrder{
             Column::Type, Column::Modified, Column::Size, Column::Accessed, Column::Created, Column::Owner,
             Column::Group, Column::Permissions, Column::Location
         })
    );
    TEST_CHECK(
        (files::moveDetailsColumn(defaults, Column::Type, 1, {.size = false})
         == files::DetailsColumnOrder{
             Column::Modified, Column::Size, Column::Type, Column::Accessed, Column::Created, Column::Owner,
             Column::Group, Column::Permissions, Column::Location
         })
    );
    TEST_CHECK(
        !files::moveDetailsColumn(defaults, Column::Type, -1, {.type = false})
        && !files::moveDetailsColumn(defaults, Column::Type, 1, {.size = false, .modified = false})
        && !files::moveDetailsColumn(defaults, Column::Type, 0, {})
        && !files::moveDetailsColumn({Column::Type, Column::Type, Column::Size}, Column::Type, 1, {})
    );
  }

  void testDetailsColumnInsertion() {
    using Column = files::DetailsColumn;
    const auto defaults = files::kDefaultDetailsColumnOrder;
    TEST_CHECK(
        (files::insertDetailsColumnBefore(defaults, Column::Type, std::nullopt)
         == files::DetailsColumnOrder{
             Column::Size, Column::Modified, Column::Accessed, Column::Created, Column::Owner, Column::Group,
             Column::Permissions, Column::Location, Column::Type
         })
    );
    TEST_CHECK(
        (files::insertDetailsColumnBefore(defaults, Column::Modified, Column::Type)
         == files::DetailsColumnOrder{
             Column::Modified, Column::Type, Column::Size, Column::Accessed, Column::Created, Column::Owner,
             Column::Group, Column::Permissions, Column::Location
         })
    );
    TEST_CHECK(
        files::insertDetailsColumnBefore(defaults, Column::Type, Column::Type) == defaults
        && files::insertDetailsColumnBefore(defaults, Column::Size, Column::Modified) == defaults
    );
    TEST_CHECK(
        !files::insertDetailsColumnBefore(defaults, static_cast<Column>(99), std::nullopt)
        && !files::insertDetailsColumnBefore(defaults, Column::Type, static_cast<Column>(99))
        && !files::insertDetailsColumnBefore({Column::Type, Column::Type, Column::Size}, Column::Type, std::nullopt)
    );
  }

  void testTypeSort() {
    DirectoryModel model;
    auto data = snapshot(
        {entry(10, "z-folder", files::FileType::Directory), entry(2, "link"), entry(3, "note10.txt"),
         entry(4, "note2.txt"), entry(5, "z.png"), entry(6, "a.bin"), entry(7, "b"),
         entry(8, "special", files::FileType::Special), entry(9, "c", files::FileType::Unknown)}
    );
    data.entries[1].symlink = true;
    data.entries[1].contentType = "image/png";
    data.entries[2].contentType = "text/plain";
    data.entries[3].contentType = "text/plain";
    data.entries[4].contentType = "image/png";
    data.entries[5].contentType = "application/octet-stream";
    model.applySnapshot(data);
    model.select(3);
    model.select(5, SelectionMode::Toggle);
    model.setScrollAnchor(3);
    model.setSort({.field = SortField::Type, .directoriesFirst = false});
    rows(model, {10, 2, 4, 3, 5, 8, 6, 7, 9});
    TEST_CHECK(
        model.typeLabel(10) == "Folder"
        && model.typeLabel(2) == "Link"
        && model.typeLabel(3) == model.typeLabel(4)
        && model.typeLabel(3) != model.typeLabel(5)
        && model.typeLabel(6) == "File"
        && model.typeLabel(9) == "Unknown"
    );
    model.setSort({.field = SortField::Type, .descending = true, .directoriesFirst = false});
    rows(model, {8, 5, 3, 4, 2, 10, 9, 7, 6});
    selected(model, {5, 3});
    TEST_CHECK(model.selection().focused == 5 && model.selection().anchor == 5 && model.scrollAnchor() == 3);
    std::ranges::reverse(data.entries);
    model.applySnapshot(data);
    rows(model, {8, 5, 3, 4, 2, 10, 9, 7, 6});
    model.setSort({.field = SortField::Type, .descending = true});
    rows(model, {10, 8, 5, 3, 4, 2, 9, 7, 6});
    for (auto& item : data.entries) {
      if (item.id == 3) {
        item.contentType = "image/png";
      }
    }
    model.applySnapshot(data);
    TEST_CHECK(model.typeLabel(3) == model.typeLabel(5));
    selected(model, {5, 3});
  }

  void testSelectionSummary() {
    DirectoryModel model;
    auto data = snapshot(
        {entry(1, "empty"), entry(2, "known"), entry(3, "unknown"), entry(4, "folder", files::FileType::Directory),
         entry(5, "link"), entry(6, "special", files::FileType::Special), entry(7, ".hidden")}
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
    TEST_CHECK(model.selectionSummary() == expected);
    model.setFilter({.showHidden = true, .text = {}});
    model.selectAll();
    TEST_CHECK(model.selectionSummary().bytes == 17 && model.selectionSummary().files == 4);
    model.setFilter({});
    TEST_CHECK(model.selectionSummary() == expected);
    data.entries[1].size = 10;
    model.applySnapshot(data);
    TEST_CHECK(model.selectionSummary().bytes == 10);
    TEST_CHECK(model.replaceSelection(std::array<EntryId, 2>{1, 2}));
    TEST_CHECK((model.selectionSummary() == files::SelectionSummary{.files = 2, .bytes = 10}));
    data.status = DirectoryStatus::Cancelled;
    model.applySnapshot(data);
    TEST_CHECK(model.selectionSummary() == files::SelectionSummary{});
    auto huge = snapshot({entry(10, "large"), entry(11, "one")}, 2);
    huge.entries[0].size = std::numeric_limits<std::uint64_t>::max();
    huge.entries[1].size = 1;
    model.applySnapshot(huge);
    model.selectAll();
    TEST_CHECK(
        (model.selectionSummary()
         == files::SelectionSummary{.files = 2, .bytes = std::numeric_limits<std::uint64_t>::max(), .overflow = true})
    );
    model.clearSelection();
    TEST_CHECK(model.selectionSummary() == files::SelectionSummary{});
  }

  void testFiltering() {
    DirectoryModel model;
    auto data = snapshot({entry(1, "Straße"), entry(2, "café"), entry(3, ".hidden"), entry(4, "*.txt")});
    data.entries[2].hidden = true;
    model.applySnapshot(data);
    TEST_CHECK(model.rows().size() == 3);
    model.setFilter({.text = "STRASSE"});
    rows(model, {1});
    model.setFilter({.text = "CAFE\u0301"});
    rows(model, {2});
    model.setFilter({.text = "*"});
    rows(model, {4});
    model.setFilter({.showHidden = true, .text = "hidden"});
    rows(model, {3});
    TEST_CHECK(model.entry(3)->name == ".hidden");
    TEST_CHECK(model.snapshot().entries.size() == 4);
    model.setFilter({.text = "absent"});
    rows(model, {});
    TEST_CHECK(model.snapshot().status == DirectoryStatus::Ready);
  }

  void testSelectionAndAnchors() {
    DirectoryModel model;
    model.applySnapshot(snapshot({entry(1, "a"), entry(2, "b"), entry(3, "c"), entry(4, "d")}));
    model.select(2);
    model.setScrollAnchor(2);
    model.setSort({.descending = true});
    selected(model, {2});
    TEST_CHECK(model.selection().focused == 2 && model.selection().anchor == 2 && model.scrollAnchor() == 2);
    model.select(4, SelectionMode::Range);
    selected(model, {4, 3, 2});
    TEST_CHECK(model.selection().anchor == 2 && model.selection().focused == 4);
    model.select(3, SelectionMode::Range);
    selected(model, {3, 2});
    model.select(1, SelectionMode::Toggle);
    selected(model, {3, 2, 1});
    model.select(1, SelectionMode::Toggle);
    selected(model, {3, 2});
    TEST_CHECK(model.selection().anchor == 1 && model.selection().focused == 1);
    model.select(4, SelectionMode::AddRange);
    selected(model, {4, 3, 2, 1});
    model.clearSelection();
    TEST_CHECK(model.selection().focused == 4 && !model.selection().anchor);
    model.setFocus(3);
    model.select(1, SelectionMode::Range);
    selected(model, {3, 2, 1});
    TEST_CHECK(model.selection().anchor == 3);
    TEST_CHECK(!model.select(999) && !model.setFocus(999) && !model.setScrollAnchor(999));
    selected(model, {3, 2, 1});
    model.setFilter({.text = "b"});
    selected(model, {2});
    TEST_CHECK(!model.selection().focused && !model.selection().anchor);
    TEST_CHECK(model.scrollAnchor() == 2);
    TEST_CHECK(!model.select(1));
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
    TEST_CHECK(model.replaceSelection(initial));
    selected(model, {1, 3});
    TEST_CHECK(publications == 1 && model.selection().anchor == 1 && model.selection().focused == 1);
    const std::array invalid{files::EntryId{2}, files::EntryId{999}};
    TEST_CHECK(!model.replaceSelection(invalid));
    selected(model, {1, 3});
    TEST_CHECK(publications == 1);
    const std::array reduced{files::EntryId{3}};
    TEST_CHECK(model.replaceSelection(reduced));
    selected(model, {3});
    model.setSort({.descending = true});
    TEST_CHECK(model.replaceSelection(initial));
    selected(model, {3, 1});
    model.setFilter({.text = "c"});
    TEST_CHECK(!model.replaceSelection(initial));
    selected(model, {3});
    TEST_CHECK(model.replaceSelection({}) && !model.selection().anchor);
    selected(model, {});
    auto stopped = model.snapshot();
    stopped.status = DirectoryStatus::Cancelled;
    model.applySnapshot(stopped);
    TEST_CHECK(!model.replaceSelection(reduced));
  }

  void testSnapshotReconciliation() {
    DirectoryModel model;
    auto data = snapshot({entry(1, "a"), entry(2, "b")}, 10);
    model.applySnapshot(data);
    model.select(1);
    model.setScrollAnchor(1);
    auto loading = data;
    loading.generation = 11;
    loading.status = DirectoryStatus::Loading;
    model.applySnapshot(loading);
    selected(model, {1});
    TEST_CHECK(!model.applySnapshot(data));
    TEST_CHECK(model.snapshot().status == DirectoryStatus::Loading);
    data.generation = 11;
    data.entries[0] = entry(1, "z-renamed");
    model.applySnapshot(data);
    selected(model, {1});
    TEST_CHECK(model.entry(1)->name == "z-renamed" && model.scrollAnchor() == 1);
    data.generation = 12;
    data.entries[0] = entry(3, "z-renamed");
    model.applySnapshot(data);
    selected(model, {});
    TEST_CHECK(!model.selection().focused && !model.selection().anchor && !model.scrollAnchor());
    model.select(2);
    auto cancelled = data;
    cancelled.generation = 13;
    cancelled.status = DirectoryStatus::Cancelled;
    model.applySnapshot(cancelled);
    selected(model, {});
    TEST_CHECK(!model.select(2));
    cancelled.generation = 14;
    cancelled.status = DirectoryStatus::Loading;
    model.applySnapshot(cancelled);
    TEST_CHECK(!model.select(2));
    cancelled.status = DirectoryStatus::Error;
    cancelled.error = files::FileError{.kind = files::FileErrorKind::Denied, .message = "partial fixture"};
    model.applySnapshot(cancelled);
    TEST_CHECK(model.snapshot().error && model.select(2));
    auto other = snapshot({entry(2, "other")}, 15);
    other.location = Location::fromPath("/tmp/another-model-location");
    model.applySnapshot(other);
    selected(model, {});
    TEST_CHECK(!model.selection().focused);
    auto dying = std::make_unique<DirectoryModel>();
    dying->setChangedCallback([&dying] { dying.reset(); });
    dying->applySnapshot(other);
  }

  struct Fixture {
    std::filesystem::path path;
    Fixture() {
      const files::GString directory(g_dir_make_tmp("noctalia-model-test-XXXXXX", nullptr));
      TEST_CHECK(directory != nullptr);
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
      TEST_CHECK(file.good());
    }
  };

  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        const int timeout = requested < 0 ? 20 : std::min(requested, 20);
        TEST_CHECK(::poll(fds.data(), fds.size(), timeout) >= 0);
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
    pump.until([&] { return model.snapshot().status == DirectoryStatus::Ready; });
    TEST_CHECK(model.rows().size() == 2);
    const auto id = model.rows().front();
    const auto otherId = model.rows().back();
    TEST_CHECK(model.entry(id)->name == name);
    model.select(id);
    model.setScrollAnchor(id);
    std::filesystem::rename(fixture.path / name, fixture.path / "item20");
    pump.until([&] { return model.entry(id) && model.entry(id)->name == "item20"; });
    rows(model, {otherId, id});
    selected(model, {id});
    TEST_CHECK(model.selection().focused == id && model.scrollAnchor() == id);
    session.refresh();
    selected(model, {id});
    pump.until([&] { return model.snapshot().status == DirectoryStatus::Ready; });
    selected(model, {id});
    std::filesystem::remove(fixture.path / "item20");
    pump.until([&] { return model.entry(id) == nullptr; });
    selected(model, {});
    fixture.write("item20");
    pump.until([&] { return model.rows().size() == 2; });
    TEST_CHECK(model.entry(id) == nullptr && model.selection().selected.empty());
  }
} // namespace

int main() {
  try {
    // MIME descriptions consult language environment variables independently of collation.
    g_setenv("LC_ALL", "C", true);
    g_setenv("LANGUAGE", "C", true);
    std::setlocale(LC_ALL, "C");
    testSortAndMetadata();
    testExtendedMetadataSort();
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
