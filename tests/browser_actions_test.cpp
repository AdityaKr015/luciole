#include "browser/action_registry.h"
#include "browser/selection_tools.h"

#include <algorithm>
#include <chrono>
#include <print>
#include <stdexcept>

namespace {
  using namespace std::chrono_literals;
  using A = files::BrowserAction;
  using R = files::ActionRegistry;
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
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
  void testPreviewActions() {
    files::DirectoryModel model;
    model.applySnapshot(snapshot());
    files::ActionContext context{.model = model, .context = 1, .query = {}, .pinned = {}};
    require(!R::state(A::QUICK_PREVIEW, context, R::capture(context)).enabled, "empty preview target accepted");
    model.select(1);
    const auto request = R::capture(context);
    require(
        R::state(A::OPEN_WITH, context, request).enabled && R::state(A::QUICK_PREVIEW, context, request).enabled,
        "single-file preview or application choice refused"
    );
    model.select(6);
    require(
        !R::state(A::OPEN_WITH, context, R::capture(context)).enabled
            && R::state(A::QUICK_PREVIEW, context, R::capture(context)).enabled,
        "folder opening choices are incorrect"
    );
    model.selectAll();
    require(!R::state(A::QUICK_PREVIEW, context, R::capture(context)).enabled, "multiple preview targets accepted");
    ++context.context;
    require(!R::state(A::OPEN_WITH, context, request).enabled, "retired application-choice target accepted");
  }
  void testArchiveActions() {
    files::DirectoryModel model;
    auto data = snapshot();
    data.entries.push_back(entry(7, "sample.TAR.GZ"));
    model.applySnapshot(data);
    model.select(7);
    files::ActionContext c{.model = model, .context = 9, .query = {}, .pinned = {}};
    const auto request = R::capture(c);
    require(
        R::state(A::COMPRESS, c, request).enabled
            && R::state(A::EXTRACT_HERE, c, request).enabled
            && R::state(A::EXTRACT_TO, c, request).enabled
            && R::state(A::BROWSE_ARCHIVE, c, request).enabled,
        "native archive actions unavailable"
    );
    c.operationsBusy = true;
    require(
        !R::state(A::BROWSE_ARCHIVE, c, request).enabled && !R::state(A::COMPRESS, c, request).enabled,
        "archive jobs bypassed operation ownership"
    );
    c.operationsBusy = false;
    data.entries.back().symlink = true;
    ++data.generation;
    model.applySnapshot(data);
    require(!R::state(A::EXTRACT_TO, c, request).enabled, "archive symlink authorized extraction");
    data.entries.back().symlink = false;
    data.entries.back().access.read = false;
    ++data.generation;
    model.applySnapshot(data);
    require(!R::state(A::COMPRESS, c, request).enabled, "unreadable archive authorized compression");
    model.select(1);
    const auto plain = R::capture(c);
    require(
        R::state(A::COMPRESS, c, plain).enabled && !R::state(A::BROWSE_ARCHIVE, c, plain).enabled,
        "ordinary file archive capabilities incorrect"
    );
    model.select(6);
    require(R::state(A::COMPRESS, c, R::capture(c)).enabled, "native directory could not be compressed");
    data.entries.back().location = files::Location::fromUri("sftp://example.invalid/sample.tar.gz");
    data.entries.back().access.read = true;
    ++data.generation;
    model.applySnapshot(data);
    model.select(7);
    require(!R::state(A::COMPRESS, c, R::capture(c)).enabled, "remote archive advertised native operations");
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
    require(
        R::state(A::OPEN, c, request).enabled
            && R::state(A::RENAME, c, request).enabled
            && R::state(A::TRASH, c, request).enabled,
        "unknown access facts wrongly disabled native operations"
    );
    model.select(3);
    require(
        R::state(A::RENAME, c, request).enabled && request.targets.front().id == 2,
        "a captured menu was retargeted to a later selection"
    );
    model.setSort({files::SortField::NAME, true, false});
    require(R::state(A::OPEN, c, request).enabled, "sort invalidated a live stable target");
    c.operationsBusy = true;
    const auto busy = R::state(A::RENAME, c, request);
    require(
        !busy.enabled && !busy.reason.empty() && R::state(A::PROPERTIES, c, request).enabled,
        "job ownership did not consistently gate mutations while retaining metadata access"
    );
    c.operationsBusy = false;
    data.metadata.filesystem.readOnly = true;
    ++data.generation;
    model.applySnapshot(data);
    require(
        !R::state(A::RENAME, c, request).enabled && !R::state(A::CREATE_FILE, c, request).enabled,
        "reported read-only filesystem permitted mutation"
    );
    data.metadata.filesystem.readOnly = false;
    data.entries[1].access.rename = false;
    data.entries[1].access.trash = false;
    data.entries[1].access.read = false;
    ++data.generation;
    model.applySnapshot(data);
    require(
        !R::state(A::RENAME, c, request).enabled
            && !R::state(A::TRASH, c, request).enabled
            && !R::state(A::OPEN, c, request).enabled,
        "fresh provider denials were ignored by a captured request"
    );
    data.entries[1].access = {};
    data.metadata.access.write = false;
    ++data.generation;
    model.applySnapshot(data);
    require(!R::state(A::CREATE_FOLDER, c, request).enabled, "folder write denial was ignored");
    c.query = "txt";
    require(!R::state(A::OPEN, c, request).enabled, "query change did not retire a menu");
    auto search = R::capture(c);
    search.targets = request.targets;
    require(
        !R::state(A::TRASH, c, search).enabled && R::state(A::RENAME, c, search).enabled,
        "native search result operation limits changed"
    );
    c.query = {};
    ++c.context;
    require(!R::state(A::OPEN, c, request).enabled, "leaving and returning to a context revived an old request");
    c.context = request.context;
    data.entries[1].location = files::Location::fromPath("/tmp/action-fixture/renamed.txt");
    ++data.generation;
    model.applySnapshot(data);
    require(!R::state(A::OPEN, c, request).enabled, "renamed operation address accepted an old menu target");
    data.entries.erase(data.entries.begin() + 1);
    ++data.generation;
    model.applySnapshot(data);
    require(!R::state(A::PROPERTIES, c, request).enabled, "deleted target was accepted");
    data.entries.push_back(entry(20, "Beta.txt"));
    ++data.generation;
    model.applySnapshot(data);
    require(!R::state(A::OPEN, c, request).enabled, "replacement at the same URI inherited a menu target");
    model.select(6);
    const auto folder = R::capture(c);
    require(
        R::state(A::PIN, c, folder).enabled && !R::state(A::UNPIN, c, folder).enabled,
        "pin capabilities disagreed with current pins"
    );
    c.pinned = [](const files::Location&) { return true; };
    require(
        !R::state(A::PIN, c, folder).enabled && R::state(A::UNPIN, c, folder).enabled, "unpin did not recheck pin state"
    );
    model.selectAll();
    const auto multiple = R::capture(c);
    require(
        !R::state(A::OPEN, c, multiple).enabled
            && !R::state(A::RENAME, c, multiple).enabled
            && R::state(A::PROPERTIES, c, multiple).enabled,
        "mixed selection capabilities were inconsistent"
    );
    data.status = files::DirectoryStatus::CANCELLED;
    ++data.generation;
    model.applySnapshot(data);
    require(!R::state(A::SELECT_ALL, c, R::capture(c)).enabled, "cancelled listings still allowed selection actions");
    auto remote = snapshot();
    remote.location = files::Location::fromUri("sftp://example.test/files");
    remote.entries = {entry(90, "remote")};
    remote.entries.front().location = files::Location::fromUri("sftp://example.test/files/remote");
    remote.generation = ++data.generation;
    model.applySnapshot(remote);
    model.select(90);
    require(
        !R::state(A::RENAME, c, R::capture(c)).enabled && R::state(A::OPEN, c, R::capture(c)).enabled,
        "native mutation boundaries leaked into remote read actions"
    );
  }
  void testBulkSelection() {
    files::DirectoryModel model;
    model.applySnapshot(snapshot());
    model.select(2);
    const auto focus = model.selection().focused, anchor = model.selection().anchor;
    model.invertSelection();
    require(
        model.selection().selected.size() == 5
            && !model.selection().selected.contains(2)
            && model.selection().focused == focus
            && model.selection().anchor == anchor,
        "invert changed independent focus/range identities"
    );
    model.invertSelection();
    require(model.selectedIds() == std::vector<files::EntryId>{2}, "double inversion lost the original selection");
    require(
        !model.selectPattern("*.TXT")
            && model.selection().selected.size() == 4
            && model.selection().focused == focus
            && model.selection().anchor == anchor,
        "pattern did not select case-insensitive visible names independently of focus"
    );
    require(
        !model.selectPattern("CAFE\xCC\x81.???") && model.selectedIds() == std::vector<files::EntryId>{4},
        "normalized Unicode pattern did not match composed filename"
    );
    require(
        !model.selectPattern("[literal].txt") && model.selectedIds() == std::vector<files::EntryId>{5},
        "literal brackets were treated as a different pattern language"
    );
    const auto before = model.selectedIds();
    require(
        model.selectPattern("").has_value()
            && model.selectPattern(std::string(257, '*')).has_value()
            && model.selectPattern(std::string("a\0b", 3)).has_value()
            && model.selectPattern(std::string(1, '\xFF')).has_value()
            && model.selectedIds() == before,
        "invalid pattern changed selection"
    );
    model.setFilter({false, "Beta"});
    model.selectPattern("*");
    require(model.selectedIds() == std::vector<files::EntryId>{2}, "pattern escaped the visible projection");
    model.invertSelection();
    require(model.selectedIds().empty(), "invert included filtered entries");
  }
  void testTypeahead() {
    files::DirectoryModel model;
    model.applySnapshot(snapshot());
    files::TypeaheadSelection input;
    const auto now = files::TypeaheadSelection::Clock::now();
    require(
        input.append(model, "b", now) == 2 && input.append(model, "B", now + 20ms) == 3 && input.text() == "b",
        "repeated letter did not cycle visible prefix matches"
    );
    require(input.append(model, "e", now + 40ms) == 2, "extended prefix did not select its match");
    require(
        !input.append(model, "x", now + 60ms) && model.selection().focused == 2, "unmatched prefix changed selection"
    );
    require(input.erase(model, 1, now + 80ms) == 2 && input.text() == "be", "backspace did not refine the prefix");
    require(input.append(model, "caf", now + 2s) == 4 && input.text() == "caf", "timeout did not start a new prefix");
    require(
        input.append(model, "é", now + 4s, true) == 4 && input.text() == "café",
        "long IME composition expired its existing prefix"
    );
    const auto prior = input.text();
    input.erase(model, 1, now + 4100ms);
    require(input.text() == prior, "IME deletion split a UTF-8 character");
    require(
        input.erase(model, 2, now + 4200ms) == 4 && input.text() == "caf",
        "byte-count deletion did not remove a complete Unicode character"
    );
    require(input.expire(now + 6s) && input.text().empty(), "idle prefix did not expire");
    require(
        !input.append(model, std::string(1, '\xFF'), now + 7s) && input.text().empty(), "invalid UTF-8 reached matching"
    );
    model.setFilter({false, "alpha"});
    require(!input.append(model, "b", now + 8s), "typeahead selected a hidden projection row");
  }
} // namespace
int main() {
  try {
    testArchiveActions();
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
