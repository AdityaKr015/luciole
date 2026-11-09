#include "browser/action_registry.h"
#include "browser/selection_tools.h"
#include "core/input/key_modifiers.h"
#include "fs/archive_type.h"
#include "fs/gio_interop.h"
#include "test_check.h"
#include "ui/browser/action_shortcuts.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <print>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  using namespace std::chrono_literals;
  using A = files::BrowserAction;
  using R = files::ActionRegistry;
  files::DirectoryEntry
  entry(files::EntryId id, const std::string& name, files::FileType type = files::FileType::Regular) {
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
        .status = files::DirectoryStatus::Ready,
        .entries =
            {entry(1, "alpha.txt"), entry(2, "Beta.txt"), entry(3, "boulder.png"), entry(4, "café.txt"),
             entry(5, "[literal].txt"), entry(6, "folder", files::FileType::Directory)},
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
  void testTrashActions() {
    files::DirectoryModel model;
    auto data = snapshot();
    data.location = files::Location::trash();
    data.entries.erase(data.entries.begin() + 1, data.entries.end());
    data.entries[0].location = files::Location::fromUri("trash:///alpha.txt");
    data.entries[0].identity = files::FileIdentity{"trash-volume", "item-1"};
    data.entries[0].trash.original = files::Location::fromPath("/tmp/original/alpha.txt");
    model.applySnapshot(data);
    model.select(1);
    files::ActionContext context{.model = model, .context = 33, .query = {}, .pinned = {}};
    const auto request = R::capture(context);
    TEST_CHECK(
        R::state(A::Restore, context, request).enabled && R::state(A::DeletePermanently, context, request).enabled
    );
    context.operationsBusy = true;
    TEST_CHECK(
        !R::state(A::Restore, context, request).enabled && !R::state(A::DeletePermanently, context, request).enabled
    );
    context.operationsBusy = false;
    data.entries[0].trash.original.reset();
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(
        !R::state(A::Restore, context, request).enabled && R::state(A::DeletePermanently, context, request).enabled
    );
    data.entries[0].access.remove = false;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::DeletePermanently, context, request).enabled);
    data.entries[0].access.remove = true;
    data.location = files::Location::fromUri("trash:///folder");
    data.entries[0].location = files::Location::fromUri("trash:///folder/alpha.txt");
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::DeletePermanently, context, R::capture(context)).enabled);
    data.location = files::Location::fromPath("/tmp/action-fixture");
    data.entries[0].location = files::Location::fromPath("/tmp/action-fixture/alpha.txt");
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::DeletePermanently, context, R::capture(context)).enabled);
  }
  void testLinkActions() {
    files::DirectoryModel model;
    auto data = snapshot();
    data.entries[0].identity = files::FileIdentity{"native", "source"};
    model.applySnapshot(data);
    model.select(1);
    files::ActionContext context{.model = model, .context = 44, .query = {}, .pinned = {}};
    const auto enabled = [&] { return R::state(A::CreateLink, context, R::capture(context)).enabled; };
    TEST_CHECK(enabled());
    context.operationsBusy = true;
    TEST_CHECK(!enabled());
    context.operationsBusy = false;
    data.metadata.filesystem.readOnly = true;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(enabled());
    for (const auto type : {files::FileType::Directory, files::FileType::Symlink}) {
      data.entries[0].type = type;
      data.entries[0].symlink = type == files::FileType::Symlink;
      ++data.generation;
      model.applySnapshot(data);
      TEST_CHECK(enabled());
    }
    data.entries[0].type = files::FileType::Special;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!enabled());
    data.entries[0].type = files::FileType::Regular;
    data.entries[0].symlink = false;
    for (const auto& uri : {"trash:///source", "sftp://example.invalid/source"}) {
      data.entries[0].location = files::Location::fromUri(uri);
      ++data.generation;
      model.applySnapshot(data);
      TEST_CHECK(!enabled());
    }
    data.entries[0].location = files::Location::fromPath("/tmp/action-fixture/alpha.txt");
    data.entries[0].identity.reset();
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!enabled());
    data.entries[0].identity = files::FileIdentity{"native", "source"};
    ++data.generation;
    model.applySnapshot(data);
    context.query = "source";
    TEST_CHECK(enabled());
    model.selectAll();
    TEST_CHECK(!enabled());
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
        R::state(A::Compress, c, request).enabled
        && R::state(A::ExtractHere, c, request).enabled
        && R::state(A::ExtractTo, c, request).enabled
        && R::state(A::BrowseArchive, c, request).enabled
    );
    c.operationsBusy = true;
    TEST_CHECK(!R::state(A::BrowseArchive, c, request).enabled && !R::state(A::Compress, c, request).enabled);
    c.operationsBusy = false;
    data.entries.back().symlink = true;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::ExtractTo, c, request).enabled);
    data.entries.back().symlink = false;
    data.entries.back().access.read = false;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::Compress, c, request).enabled);
    model.select(1);
    const auto plain = R::capture(c);
    TEST_CHECK(R::state(A::Compress, c, plain).enabled && !R::state(A::BrowseArchive, c, plain).enabled);
    model.select(6);
    TEST_CHECK(R::state(A::Compress, c, R::capture(c)).enabled);
    data.entries.back().location = files::Location::fromUri("sftp://example.invalid/sample.tar.gz");
    data.entries.back().access.read = true;
    ++data.generation;
    model.applySnapshot(data);
    model.select(7);
    TEST_CHECK(!R::state(A::Compress, c, R::capture(c)).enabled);
  }
  void testSplitArchiveActions() {
    files::DirectoryModel model;
    auto data = snapshot();
    data.entries.push_back(entry(7, "download.ZIP.001"));
    data.entries.push_back(entry(8, "download.ZIP.002"));
    data.entries.back().contentType = "application/zip";
    model.applySnapshot(data);
    model.select(7);
    files::ActionContext context{.model = model, .context = 1, .query = {}, .pinned = {}};
    auto request = R::capture(context);
    for (const auto action : {A::BrowseArchive, A::ExtractHere, A::ExtractTo}) {
      TEST_CHECK(R::state(action, context, request).enabled);
    }
    model.select(8);
    request = R::capture(context);
    for (const auto action : {A::BrowseArchive, A::ExtractHere, A::ExtractTo}) {
      TEST_CHECK(!R::state(action, context, request).enabled);
    }
    TEST_CHECK(files::archiveFolderName(data.entries[6].location) == "download");
    TEST_CHECK(!files::isSupportedArchive(files::Location::fromUri("sftp://example.invalid/test.zip.001"), {}));
    TEST_CHECK(!files::splitZipPart(files::Location::fromPath("/tmp/test.zip.01")));
    TEST_CHECK(!files::splitZipPart(files::Location::fromPath("/tmp/test.zip.001.bak")));
  }
  void testCompressedArchiveActions() {
    for (const auto compression : {files::ArchiveCompression::Bzip2, files::ArchiveCompression::Zstd}) {
      const bool bzip = compression == files::ArchiveCompression::Bzip2;
      const char* const name = bzip ? "package.TAR.BZ2" : "package.TAR.ZST";
      files::DirectoryModel model;
      auto data = snapshot();
      data.entries.push_back(entry(7, name));
      model.applySnapshot(data);
      model.select(7);
      files::ActionContext context{.model = model, .context = 1, .query = {}, .pinned = {}};
      const auto request = R::capture(context);
      for (const auto action : {A::BrowseArchive, A::ExtractHere, A::ExtractTo}) {
        TEST_CHECK(R::state(action, context, request).enabled == files::archiveCompressionAvailable(compression));
      }
      TEST_CHECK(files::archiveFolderName(data.entries.back().location) == "package");
      const auto opaque = files::Location::fromPath("/tmp/opaque-container");
      TEST_CHECK(
          files::isSupportedArchive(
              opaque, bzip ? "application/x-bzip2-compressed-tar" : "application/x-zstd-compressed-tar"
          )
          == files::archiveCompressionAvailable(compression)
      );
    }
    for (const char* const name : {"a.tbz2", "a.tbz", "a.tzst"}) {
      TEST_CHECK(files::archiveFolderName(files::Location::fromPath(std::string("/tmp/") + name)) == "a");
    }
    TEST_CHECK(!files::isSupportedArchive(files::Location::fromPath("/tmp/plain.bz2"), "application/x-bzip2"));
    TEST_CHECK(!files::isSupportedArchive(files::Location::fromPath("/tmp/plain.zst"), "application/zstd"));
    TEST_CHECK(
        !files::isSupportedArchive(files::Location::fromPath("/tmp/untested.tar.xz"), "application/x-xz-compressed-tar")
    );
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
        R::state(A::Open, c, request).enabled
        && R::state(A::Rename, c, request).enabled
        && R::state(A::Trash, c, request).enabled
        && R::state(A::NewDocument, c, request).enabled
    );
    model.select(3);
    TEST_CHECK(R::state(A::Rename, c, request).enabled && request.targets.front().id == 2);
    model.setSort({files::SortField::Name, true, false});
    TEST_CHECK(R::state(A::Open, c, request).enabled);
    c.operationsBusy = true;
    const auto busy = R::state(A::Rename, c, request);
    TEST_CHECK(!R::state(A::NewDocument, c, request).enabled);
    TEST_CHECK(!busy.enabled && !busy.reason.empty() && R::state(A::Properties, c, request).enabled);
    c.operationsBusy = false;
    data.metadata.filesystem.readOnly = true;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(
        !R::state(A::Rename, c, request).enabled
        && !R::state(A::CreateFile, c, request).enabled
        && !R::state(A::NewDocument, c, request).enabled
    );
    data.metadata.filesystem.readOnly = false;
    data.entries[1].access.rename = false;
    data.entries[1].access.trash = false;
    data.entries[1].access.read = false;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(
        !R::state(A::Rename, c, request).enabled
        && !R::state(A::Trash, c, request).enabled
        && !R::state(A::Open, c, request).enabled
    );
    data.entries[1].access = {};
    data.metadata.access.write = false;
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::CreateFolder, c, request).enabled && !R::state(A::NewDocument, c, request).enabled);
    c.query = "txt";
    TEST_CHECK(!R::state(A::Open, c, request).enabled);
    auto search = R::capture(c);
    search.targets = request.targets;
    TEST_CHECK(!R::state(A::Trash, c, search).enabled && R::state(A::Rename, c, search).enabled);
    c.query = {};
    ++c.context;
    TEST_CHECK(!R::state(A::Open, c, request).enabled);
    c.context = request.context;
    data.entries[1].location = files::Location::fromPath("/tmp/action-fixture/renamed.txt");
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::Open, c, request).enabled);
    data.entries.erase(data.entries.begin() + 1);
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::Properties, c, request).enabled);
    data.entries.push_back(entry(20, "Beta.txt"));
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::Open, c, request).enabled);
    model.select(6);
    const auto folder = R::capture(c);
    TEST_CHECK(R::state(A::Pin, c, folder).enabled && !R::state(A::Unpin, c, folder).enabled);
    TEST_CHECK(R::state(A::PinCurrent, c, folder).enabled && !R::state(A::UnpinCurrent, c, folder).enabled);
    c.pinned = [](const files::Location&) { return true; };
    TEST_CHECK(!R::state(A::Pin, c, folder).enabled && R::state(A::Unpin, c, folder).enabled);
    TEST_CHECK(!R::state(A::PinCurrent, c, folder).enabled && R::state(A::UnpinCurrent, c, folder).enabled);
    for (auto& item : data.entries) {
      item.identity = files::FileIdentity{"native", std::to_string(item.id)};
    }
    ++data.generation;
    model.applySnapshot(data);
    model.selectAll();
    const auto multiple = R::capture(c);
    TEST_CHECK(
        !R::state(A::Open, c, multiple).enabled
        && R::state(A::Rename, c, multiple).enabled
        && R::state(A::Properties, c, multiple).enabled
    );
    data.entries.front().location = files::Location::fromPath("/tmp/another-folder/alpha");
    ++data.generation;
    model.applySnapshot(data);
    TEST_CHECK(!R::state(A::Rename, c, R::capture(c)).enabled);
    data.status = files::DirectoryStatus::Cancelled;
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
    TEST_CHECK(!R::state(A::Rename, c, R::capture(c)).enabled && R::state(A::Open, c, R::capture(c)).enabled);
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
  void testActionTable() {
    TEST_CHECK(R::definitions().size() == files::kBrowserActionCount);
    for (std::size_t index = 0; index < files::kBrowserActionCount; ++index) {
      const auto action = R::definitions()[index].action;
      TEST_CHECK(&R::definition(action) == &R::definitions()[index]);
      TEST_CHECK(R::fromMenuId(R::menuId(action)) == action);
    }
    TEST_CHECK(R::definition(A::MenuBack).label == "Back");
    TEST_CHECK(!R::fromMenuId(-1));
    TEST_CHECK(!R::fromMenuId(static_cast<int>(files::kBrowserActionCount)));
    TEST_CHECK(!R::fromMenuId(9000));
  }
  void testShortcuts() {
    for (const auto& definition : R::definitions()) {
      TEST_CHECK(files::actionChord(definition.action).has_value() == !definition.shortcut.empty());
    }
    const auto chord = [](A action) { return files::actionChord(action).value_or(KeyChord{}); };
    TEST_CHECK((chord(A::Settings) == KeyChord{XKB_KEY_comma, KeyMod::Ctrl}));
    TEST_CHECK((chord(A::Back) == KeyChord{XKB_KEY_Left, KeyMod::Alt}));
    TEST_CHECK((chord(A::Open) == KeyChord{XKB_KEY_Return, 0}));
    TEST_CHECK((chord(A::QuickPreview) == KeyChord{XKB_KEY_space, 0}));
    TEST_CHECK((chord(A::Properties) == KeyChord{XKB_KEY_Return, KeyMod::Alt}));
    TEST_CHECK((chord(A::EditLocation) == KeyChord{XKB_KEY_l, KeyMod::Ctrl}));
    TEST_CHECK((chord(A::CreateFolder) == KeyChord{XKB_KEY_n, KeyMod::Ctrl | KeyMod::Shift}));
    TEST_CHECK(
        files::shortcutMatches(A::CreateFolder, XKB_KEY_N, KeyMod::Ctrl | KeyMod::Shift)
        && files::shortcutMatches(A::CreateFolder, XKB_KEY_n, KeyMod::Ctrl | KeyMod::Shift)
        && !files::shortcutMatches(A::CreateFolder, XKB_KEY_n, KeyMod::Ctrl)
    );
    TEST_CHECK(
        files::shortcutMatches(A::CopyFiles, XKB_KEY_c, KeyMod::Ctrl)
        && !files::shortcutMatches(A::CopyFiles, XKB_KEY_C, KeyMod::Ctrl)
        && !files::shortcutMatches(A::CopyFiles, XKB_KEY_c, KeyMod::Ctrl | KeyMod::Alt)
    );
    TEST_CHECK(!files::shortcutMatches(A::Root, 0, 0));
    TEST_CHECK(files::shortcutAction({A::Grid, A::List, A::Miller}, XKB_KEY_2, KeyMod::Ctrl) == A::List);
    TEST_CHECK(files::shortcutAction({A::DeletePermanently, A::Trash}, XKB_KEY_Delete, 0) == A::DeletePermanently);
    TEST_CHECK(!files::shortcutAction({A::Back, A::Forward}, XKB_KEY_Left, 0));
  }
} // namespace
int main() {
  try {
    testActionTable();
    testShortcuts();
    testArchiveActions();
    testSplitArchiveActions();
    testCompressedArchiveActions();
    testTrashActions();
    testLinkActions();
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
