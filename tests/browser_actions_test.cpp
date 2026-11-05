#include "browser/action_registry.h"
#include "browser/selection_tools.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <print>
#include <stdexcept>

namespace {
  using namespace std::chrono_literals;
  using A = files::BrowserAction;
  using R = files::ActionRegistry;
  files::DirectoryEntry entry(files::EntryId id, const std::string& name, GFileType type = G_FILE_TYPE_REGULAR) {
    return {
        .id = id,
        .location = files::Location::fromPath("/tmp/action-fixture/" + name),
        .name = name,
        .displayName = name,
        .type = type,
        .size = {},
        .modifiedSeconds = {},
        .etag = {},
        .parentDisplayName = {}
    };
  }
  files::DirectorySnapshot snapshot() {
    return {
        .generation = 1,
        .location = files::Location::fromPath("/tmp/action-fixture"),
        .status = files::DirectoryStatus::READY,
        .entries =
            {entry(1, "alpha.txt"), entry(2, "Beta.txt"), entry(3, "boulder.png"), entry(4, "café.txt"),
             entry(5, "[literal].txt"), entry(6, "folder", G_FILE_TYPE_DIRECTORY)},
        .error = {},
        .monitorError = {}
    };
  }
  void testFileClipboardActions() {
    files::DirectoryModel model;
    auto data = snapshot();
    model.applySnapshot(data);
    files::ActionContext c{
        .model = model, .context = 12, .query = {}, .pinned = {}, .clipboardAvailable = true, .clipboardHasFiles = true
    };
    TEST_CHECK(!R::state(A::CopyFiles, c, R::capture(c)).enabled);
    model.select(1);
    const auto request = R::capture(c);
    TEST_CHECK(
        R::state(A::CutFiles, c, request).enabled
        && R::state(A::CopyFiles, c, request).enabled
        && R::state(A::PasteFiles, c, request).enabled
    );
    c.clipboardHasFiles = false;
    TEST_CHECK(!R::state(A::PasteFiles, c, request).enabled);
    c.clipboardHasFiles = true;
    c.operationsBusy = true;
    TEST_CHECK(!R::state(A::PasteFiles, c, request).enabled);
    c.operationsBusy = false;
    c.query = "search";
    TEST_CHECK(!R::state(A::PasteFiles, c, R::capture(c)).enabled);
    c.query = {};
    data.metadata.access.write = false;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::PasteFiles, c, request).enabled);
    data.entries[0].access.rename = false;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::CutFiles, c, request).enabled && R::state(A::CopyFiles, c, request).enabled);
    data.entries[0].location = files::Location::fromUri("sftp://example.invalid/source");
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::CopyFiles, c, R::capture(c)).enabled);
  }
  void testPreviewActions() {
    files::DirectoryModel model;
    model.applySnapshot(snapshot());
    files::ActionContext context{.model = model, .context = 1, .query = {}, .pinned = {}};
    TEST_CHECK(!R::state(A::QuickPreview, context, R::capture(context)).enabled);
    model.select(1);
    const auto request = R::capture(context);
    TEST_CHECK(R::state(A::OpenWith, context, request).enabled && R::state(A::QuickPreview, context, request).enabled);
    model.select(6);
    TEST_CHECK(
        !R::state(A::OpenWith, context, R::capture(context)).enabled
        && R::state(A::QuickPreview, context, R::capture(context)).enabled
    );
    model.selectAll();
    TEST_CHECK(!R::state(A::QuickPreview, context, R::capture(context)).enabled);
    ++context.context;
    TEST_CHECK(!R::state(A::OpenWith, context, request).enabled);
  }
  void testArchiveActions() {
    files::DirectoryModel model;
    auto data = snapshot();
    data.entries.push_back(entry(7, "sample.TAR.GZ"));
    model.applySnapshot(data);
    model.select(7);
    files::ActionContext c{.model = model, .context = 9, .query = {}, .pinned = {}};
    const auto request = R::capture(c);
    TEST_CHECK(
        R::state(A::COMPRESS, c, request).enabled
        && R::state(A::ExtractHere, c, request).enabled
        && R::state(A::ExtractTo, c, request).enabled
        && R::state(A::BrowseArchive, c, request).enabled
    );
    c.operationsBusy = true;
    TEST_CHECK(!R::state(A::BrowseArchive, c, request).enabled && !R::state(A::COMPRESS, c, request).enabled);
    c.operationsBusy = false;
    data.entries.back().symlink = true;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::ExtractTo, c, request).enabled);
    data.entries.back().symlink = false;
    data.entries.back().access.read = false;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::COMPRESS, c, request).enabled);
    model.select(1);
    const auto plain = R::capture(c);
    TEST_CHECK(R::state(A::COMPRESS, c, plain).enabled && !R::state(A::BrowseArchive, c, plain).enabled);
    model.select(6);
    TEST_CHECK(R::state(A::COMPRESS, c, R::capture(c)).enabled);
    data.entries.back().location = files::Location::fromUri("sftp://example.invalid/sample.tar.gz");
    data.entries.back().access.read = true;
    ++data.generation;
    model.applySnapshot(data);
    model.select(7);
    TEST_CHECK(!R::state(A::COMPRESS, c, R::capture(c)).enabled);
  }
  void testCapabilitiesAndTargets() {
    files::DirectoryModel model;
    auto data = snapshot();
    model.applySnapshot(data);
    model.select(2);
    files::ActionContext c{
        .model = model, .context = 4, .query = {}, .pinsEditable = true, .pinned = [](const files::Location&) {
          return false;
        }
    };
    const auto request = R::capture(c);
    TEST_CHECK(
        R::state(A::OPEN, c, request).enabled
        && R::state(A::RENAME, c, request).enabled
        && R::state(A::TRASH, c, request).enabled
    );
    model.select(3);
    TEST_CHECK(R::state(A::RENAME, c, request).enabled && request.targets.front().id == 2);
    model.setSort({files::SortField::NAME, true, false});
    TEST_CHECK(R::state(A::OPEN, c, request).enabled);
    c.operationsBusy = true;
    const auto busy = R::state(A::RENAME, c, request);
    TEST_CHECK(!busy.enabled && !busy.reason.empty() && R::state(A::PROPERTIES, c, request).enabled);
    c.operationsBusy = false;
    data.metadata.filesystem.readOnly = true;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::RENAME, c, request).enabled && !R::state(A::CreateFile, c, request).enabled);
    data.metadata.filesystem.readOnly = false;
    data.entries[1].access.rename = false;
    data.entries[1].access.trash = false;
    data.entries[1].access.read = false;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(
        !R::state(A::RENAME, c, request).enabled
        && !R::state(A::TRASH, c, request).enabled
        && !R::state(A::OPEN, c, request).enabled
    );
    data.entries[1].access = {};
    data.metadata.access.write = false;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::CreateFolder, c, request).enabled);
    c.query = "txt";
    TEST_CHECK(!R::state(A::OPEN, c, request).enabled);
    auto search = R::capture(c);
    search.targets = request.targets;
    TEST_CHECK(!R::state(A::TRASH, c, search).enabled && R::state(A::RENAME, c, search).enabled);
    c.query = {};
    ++c.context;
    TEST_CHECK(!R::state(A::OPEN, c, request).enabled);
    c.context = request.context;
    data.entries[1].location = files::Location::fromPath("/tmp/action-fixture/renamed.txt");
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::OPEN, c, request).enabled);
    data.entries.erase(data.entries.begin() + 1);
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::PROPERTIES, c, request).enabled);
    data.entries.push_back(entry(20, "Beta.txt"));
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::OPEN, c, request).enabled);
    model.select(6);
    const auto folder = R::capture(c);
    TEST_CHECK(R::state(A::PIN, c, folder).enabled && !R::state(A::UNPIN, c, folder).enabled);
    c.pinned = [](const files::Location&) { return true; };
    TEST_CHECK(!R::state(A::PIN, c, folder).enabled && R::state(A::UNPIN, c, folder).enabled);
    model.selectAll();
    const auto multiple = R::capture(c);
    TEST_CHECK(
        !R::state(A::OPEN, c, multiple).enabled
        && !R::state(A::RENAME, c, multiple).enabled
        && R::state(A::PROPERTIES, c, multiple).enabled
    );
    data.status = files::DirectoryStatus::CANCELLED;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::SelectAll, c, R::capture(c)).enabled);
    auto remote = snapshot();
    remote.location = files::Location::fromUri("sftp://example.test/files");
    remote.entries = {entry(90, "remote")};
    remote.entries.front().location = files::Location::fromUri("sftp://example.test/files/remote");
    remote.generation = ++data.generation;
    model.applySnapshot(remote);
    model.select(90);
    TEST_CHECK(!R::state(A::RENAME, c, R::capture(c)).enabled && R::state(A::OPEN, c, R::capture(c)).enabled);
  }
  void testBulkSelection() {
    files::DirectoryModel model;
    model.applySnapshot(snapshot());
    model.select(2);
    const auto focus = model.selection().focused, anchor = model.selection().anchor;
    model.invertSelection();
    TEST_CHECK(
        model.selection().selected.size() == 5
        && !model.selection().selected.contains(2)
        && model.selection().focused == focus
        && model.selection().anchor == anchor
    );
    model.invertSelection();
    TEST_CHECK(model.selectedIds() == std::vector<files::EntryId>{2});
    TEST_CHECK(
        !model.selectPattern("*.TXT")
        && model.selection().selected.size() == 4
        && model.selection().focused == focus
        && model.selection().anchor == anchor
    );
    TEST_CHECK(!model.selectPattern("CAFE\xCC\x81.???") && model.selectedIds() == std::vector<files::EntryId>{4});
    TEST_CHECK(!model.selectPattern("[literal].txt") && model.selectedIds() == std::vector<files::EntryId>{5});
    const auto before = model.selectedIds();
    TEST_CHECK(
        model.selectPattern("").has_value()
        && model.selectPattern(std::string(257, '*')).has_value()
        && model.selectPattern(std::string("a\0b", 3)).has_value()
        && model.selectPattern(std::string(1, '\xFF')).has_value()
        && model.selectedIds() == before
    );
    model.setFilter({false, "Beta"});
    model.selectPattern("*");
    TEST_CHECK(model.selectedIds() == std::vector<files::EntryId>{2});
    model.invertSelection();
    TEST_CHECK(model.selectedIds().empty());
  }
  void testTypeahead() {
    files::DirectoryModel model;
    model.applySnapshot(snapshot());
    files::TypeaheadSelection input;
    const auto now = files::TypeaheadSelection::Clock::now();
    TEST_CHECK(input.append(model, "b", now) == 2 && input.append(model, "B", now + 20ms) == 3 && input.text() == "b");
    TEST_CHECK(input.append(model, "e", now + 40ms) == 2);
    TEST_CHECK(!input.append(model, "x", now + 60ms) && model.selection().focused == 2);
    TEST_CHECK(input.erase(model, 1, now + 80ms) == 2 && input.text() == "be");
    TEST_CHECK(input.append(model, "caf", now + 2s) == 4 && input.text() == "caf");
    TEST_CHECK(input.append(model, "é", now + 4s, true) == 4 && input.text() == "café");
    const auto prior = input.text();
    input.erase(model, 1, now + 4100ms);
    TEST_CHECK(input.text() == prior);
    TEST_CHECK(input.erase(model, 2, now + 4200ms) == 4 && input.text() == "caf");
    TEST_CHECK(input.expire(now + 6s) && input.text().empty());
    TEST_CHECK(!input.append(model, std::string(1, '\xFF'), now + 7s) && input.text().empty());
    model.setFilter({false, "alpha"});
    TEST_CHECK(!input.append(model, "b", now + 8s));
  }
} // namespace
int main() {
  try {
    testArchiveActions();
    testFileClipboardActions();
    testPreviewActions();
    testCapabilitiesAndTargets();
    testBulkSelection();
    testTypeahead();
    std::println("browser action checks passed");
  } catch (const std::exception& error) {
    std::println(stderr, "browser action test: {}", error.what());
    return 1;
  }
}
