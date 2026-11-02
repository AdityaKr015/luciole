#include "browser/action_registry.h"

#include "fs/archive_type.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace {
  using A = files::BrowserAction;
  using S = files::ActionScope;
  constexpr auto kActions = std::to_array<files::ActionDefinition>(
      {{A::BACK, "Back", "Alt+Left"},
       {A::FORWARD, "Forward", "Alt+Right"},
       {A::UP, "Parent folder", "Alt+Up"},
       {A::HOME, "Home", "Alt+Home"},
       {A::ROOT, "File System", ""},
       {A::REFRESH, "Refresh", "F5"},
       {A::STOP, "Stop loading", ""},
       {A::EDIT_LOCATION, "Edit location", "Ctrl+L"},
       {A::SEARCH, "Search files", "Ctrl+F"},
       {A::GRID, "Grid view", "Ctrl+1"},
       {A::LIST, "List view", "Ctrl+2"},
       {A::SHOW_HIDDEN, "Show hidden files", "Ctrl+H"},
       {A::SHOW_SIDEBAR, "Show sidebar", "F9"},
       {A::SHOW_PREVIEW, "Show preview", "F8"},
       {A::QUICK_PREVIEW, "Preview", "Space", S::TARGETS},
       {A::OPEN_WITH, "Open With…", "", S::TARGETS},
       {A::SORT_NAME, "Name", ""},
       {A::SORT_TYPE, "Type", ""},
       {A::SORT_SIZE, "Size", ""},
       {A::SORT_MODIFIED, "Modified", ""},
       {A::ASCENDING, "Ascending", ""},
       {A::DESCENDING, "Descending", ""},
       {A::FOLDERS_FIRST, "Folders first", ""},
       {A::OPEN, "Open", "Enter", S::TARGETS},
       {A::CONTAINING_FOLDER, "Show in containing folder", "", S::TARGETS},
       {A::ORIGINAL_LOCATION, "Show original location", "", S::TARGETS},
       {A::PIN, "Pin to sidebar", "", S::TARGETS},
       {A::UNPIN, "Unpin", "", S::TARGETS},
       {A::CUT_FILES, "Cut", "Ctrl+X", S::TARGETS},
       {A::COPY_FILES, "Copy", "Ctrl+C", S::TARGETS},
       {A::PASTE_FILES, "Paste", "Ctrl+V", S::LOCATION},
       {A::RENAME, "Rename…", "F2", S::TARGETS},
       {A::TRASH, "Move to Trash", "Delete", S::TARGETS},
       {A::PROPERTIES, "Properties", "Alt+Return", S::TARGETS},
       {A::COMPRESS, "Compress…", "", S::TARGETS},
       {A::EXTRACT_HERE, "Extract Here", "", S::TARGETS},
       {A::EXTRACT_TO, "Extract To…", "", S::TARGETS},
       {A::BROWSE_ARCHIVE, "Browse Archive", "", S::TARGETS},
       {A::CREATE_FOLDER, "New Folder…", "Ctrl+Shift+N", S::LOCATION},
       {A::CREATE_FILE, "Empty File…", "", S::LOCATION},
       {A::PIN_CURRENT, "Pin this folder to sidebar", "", S::LOCATION},
       {A::UNPIN_CURRENT, "Unpin this folder", "", S::LOCATION},
       {A::RELOAD_PINS, "Reload pins", ""},
       {A::EMPTY_TRASH, "Empty Trash…", "", S::GLOBAL, true},
       {A::SELECT_ALL, "Select all", "Ctrl+A", S::SELECTION},
       {A::INVERT_SELECTION, "Invert selection", "Ctrl+Shift+I", S::SELECTION},
       {A::SELECT_PATTERN, "Select by pattern…", "Ctrl+S", S::SELECTION},
       {A::CLEAR_SELECTION, "Clear selection", "Escape", S::SELECTION},
       {A::SINGLE_CLICK, "Single click", ""},
       {A::DOUBLE_CLICK, "Double click", ""},
       {A::TYPE_TO_SEARCH, "Search subfolders", ""},
       {A::TYPEAHEAD, "Select by typing", ""},
       {A::SETTINGS, "Settings…", "Ctrl+,"},
       {A::ABOUT, "About Noctalia Files", ""},
       {A::VIEW_MENU, "Back", ""},
       {A::SORT_MENU, "Sort…", ""},
       {A::SELECTION_MENU, "Selection…", ""},
       {A::INTERACTION_MENU, "Interaction…", ""},
       {A::COPY_PATH, "Copy folder path", "", S::LOCATION},
       {A::COPY_URI, "Copy folder URI", "", S::LOCATION},
       {A::TERMINAL, "Open Terminal Here", "", S::LOCATION},
       {A::COPY_TARGET_PATH, "Copy path", "", S::TARGETS},
       {A::COPY_TARGET_URI, "Copy URI", "", S::TARGETS},
       {A::TARGET_TERMINAL, "Open in Terminal", "", S::TARGETS}}
  );
  files::ActionState unavailable(std::string_view reason) { return {.enabled = false, .reason = reason}; }
} // namespace

std::span<const files::ActionDefinition> files::ActionRegistry::definitions() { return kActions; }
const files::ActionDefinition& files::ActionRegistry::definition(BrowserAction action) {
  const auto* found = std::ranges::find(kActions, action, &ActionDefinition::action);
  if (found == kActions.end())
    throw std::invalid_argument("unknown browser action");
  return *found;
}
files::ActionRequest files::ActionRegistry::capture(const ActionContext& context) {
  ActionRequest request{context.context, context.model.snapshot().location, std::string(context.query), {}};
  for (const auto id : context.model.selectedIds())
    if (const auto* entry = context.model.entry(id))
      request.targets.push_back({id, entry->location});
  return request;
}
bool files::ActionRegistry::current(const ActionContext& context, const ActionRequest& request, ActionScope scope) {
  if (scope == ActionScope::GLOBAL)
    return true;
  if (request.context != context.context
      || request.folder != context.model.snapshot().location
      || request.query != context.query)
    return false;
  return scope != ActionScope::TARGETS || std::ranges::all_of(request.targets, [&](const auto& target) {
           const auto* entry = context.model.entry(target.id);
           return entry != nullptr
               && entry->location == target.location
               && std::ranges::contains(context.model.rows(), target.id);
         });
}
files::ActionState files::ActionRegistry::state(BrowserAction action, const ActionContext& c, const ActionRequest& r) {
  const auto scope = definition(action).scope;
  if (!current(c, r, scope))
    return unavailable("The folder or targets changed. Open the menu again.");
  const auto& snapshot = c.model.snapshot();
  if (scope == ActionScope::TARGETS && r.targets.empty())
    return unavailable("Select an item first.");
  if ((scope == ActionScope::TARGETS || scope == ActionScope::SELECTION) && !c.model.selectionAvailable())
    return unavailable("The listing is not available for selection.");
  const bool ready = snapshot.status == DirectoryStatus::READY || snapshot.status == DirectoryStatus::LOADING;
  const bool native = r.folder && r.folder->nativePath().has_value();
  const auto single = [&] { return r.targets.size() == 1; };
  const auto all = [&](auto predicate) {
    return std::ranges::all_of(r.targets, [&](const auto& target) { return predicate(*c.model.entry(target.id)); });
  };
  const auto anyPinned = [&](bool pinned) {
    return c.pinned
        && std::ranges::any_of(r.targets, [&](const auto& target) { return c.pinned(target.location) == pinned; });
  };
  const auto writable = [&]() -> ActionState {
    if (c.operationsBusy)
      return unavailable("Another file operation is still finishing.");
    if (snapshot.metadata.filesystem.readOnly == true)
      return unavailable("The filesystem is read-only.");
    return {};
  };
  switch (action) {
  case A::SHOW_PREVIEW:
    return {.enabled = true, .reason = {}, .checked = c.showPreview ? 1 : 0};
  case A::QUICK_PREVIEW:
    return single() ? ActionState{} : unavailable("Select one item to preview.");
  case A::OPEN_WITH:
    return single() && all([](const DirectoryEntry& entry) {
             return entry.type == G_FILE_TYPE_REGULAR && !entry.symlink && entry.access.read != false;
           })
        ? ActionState{}
        : unavailable("Select one readable file.");
  case A::COPY_PATH:
  case A::COPY_URI:
    if (!r.folder)
      return unavailable("Open a folder first.");
    [[fallthrough]];
  case A::COPY_TARGET_PATH:
  case A::COPY_TARGET_URI:
    return c.clipboardAvailable ? ActionState{} : unavailable("The application clipboard is unavailable.");
  case A::TERMINAL:
  case A::TARGET_TERMINAL:
    if (!c.terminalAvailable)
      return unavailable("Install xdg-terminal-exec to open a terminal.");
    if (action == A::TERMINAL) {
      if (!native || r.folder->isTrash())
        return unavailable("Terminals require a native folder.");
      if (snapshot.metadata.access.execute == false)
        return unavailable("Access to this folder is not permitted.");
    } else {
      if (!single() || !all([](const auto& entry) {
            return entry.type == G_FILE_TYPE_DIRECTORY
                && entry.location.nativePath().has_value()
                && entry.access.execute.value_or(true);
          }))
        return unavailable("Select one accessible native folder.");
    }
    return {};
  case A::BACK:
    return c.canBack ? ActionState{} : unavailable("There is no previous folder.");
  case A::FORWARD:
    return c.canForward ? ActionState{} : unavailable("There is no next folder.");
  case A::UP:
    return c.canUp ? ActionState{} : unavailable("This location has no parent folder.");
  case A::REFRESH:
  case A::SEARCH:
    return snapshot.location ? ActionState{} : unavailable("Open a folder first.");
  case A::STOP:
    return snapshot.status == DirectoryStatus::LOADING ? ActionState{} : unavailable("Nothing is loading.");
  case A::CUT_FILES:
  case A::COPY_FILES:
    if (!c.clipboardAvailable)
      return unavailable("The application clipboard is unavailable.");
    if (!all([&](const auto& entry) {
          return entry.location.nativePath().has_value()
              && entry.access.read.value_or(true)
              && (entry.type == G_FILE_TYPE_DIRECTORY || entry.type == G_FILE_TYPE_REGULAR || entry.symlink)
              && (action != A::CUT_FILES || entry.access.rename.value_or(true));
        }))
      return unavailable("Select accessible native files or folders outside Trash.");
    return {};
  case A::PASTE_FILES:
    if (!c.clipboardAvailable || !c.clipboardHasFiles)
      return unavailable("The clipboard has no available files.");
    [[fallthrough]];
  case A::CREATE_FOLDER:
  case A::CREATE_FILE:
    if (!native || !c.query.empty() || !ready)
      return unavailable("Open an available native folder first.");
    if (snapshot.metadata.access.write == false)
      return unavailable("Writing to this folder is not permitted.");
    return writable();
  case A::RENAME:
  case A::TRASH: {
    if (const auto state = writable(); !state.enabled)
      return state;
    if (action == A::RENAME && !single())
      return unavailable("Select exactly one item to rename.");
    if (!all([](const auto& entry) { return entry.location.nativePath().has_value(); }))
      return unavailable("This operation currently supports native files only.");
    if (action == A::TRASH && (!native || !c.query.empty() || !ready))
      return unavailable("Open the containing native folder before moving items to Trash.");
    if (!all([&](const auto& entry) {
          return (action == A::RENAME ? entry.access.rename : entry.access.trash).value_or(true);
        }))
      return unavailable("The provider does not permit this operation on every selected item.");
    return {};
  }
  case A::COMPRESS:
  case A::EXTRACT_HERE:
  case A::EXTRACT_TO:
  case A::BROWSE_ARCHIVE:
    if (c.operationsBusy)
      return unavailable("Another file operation is still finishing.");
    if (!all([](const auto& entry) {
          return entry.location.nativePath().has_value() && entry.access.read.value_or(true);
        }))
      return unavailable("Archives require readable native items.");
    if (action == A::COMPRESS) {
      if (!all([](const auto& entry) {
            return entry.type == G_FILE_TYPE_REGULAR || entry.type == G_FILE_TYPE_DIRECTORY || entry.symlink;
          }))
        return unavailable("Special files cannot be compressed.");
    } else if (!single() || !all([](const auto& entry) {
                 return entry.type == G_FILE_TYPE_REGULAR
                     && !entry.symlink
                     && isSupportedArchive(entry.location, entry.contentType);
               }))
      return unavailable("Select one ZIP or TAR archive.");
    return {};
  case A::OPEN:
    if (!single() && !all([](const auto& entry) { return entry.type == G_FILE_TYPE_REGULAR; }))
      return unavailable("Open folders individually.");
    if (!all([](const auto& entry) {
          return !entry.location.isTrash() || (entry.type == G_FILE_TYPE_DIRECTORY && !entry.symlink);
        }))
      return unavailable("Restore this item before opening it.");
    if (!all([](const auto& entry) { return entry.access.read.value_or(true); }))
      return unavailable("Reading this item is not permitted.");
    return {};
  case A::CONTAINING_FOLDER:
  case A::ORIGINAL_LOCATION:
    if (!single())
      return unavailable("Select exactly one item.");
    if (action == A::ORIGINAL_LOCATION && !c.model.entry(r.targets.front().id)->trash.original)
      return unavailable("The original location is unavailable.");
    return {};
  case A::PIN:
  case A::UNPIN:
    if (!all([](const auto& entry) { return entry.type == G_FILE_TYPE_DIRECTORY && !entry.location.isTrash(); }))
      return unavailable("Only folders outside Trash can be pinned.");
    if (!c.pinsEditable)
      return unavailable("Reload valid pins before editing them, or wait for the save to finish.");
    return anyPinned(action == A::UNPIN)
        ? ActionState{}
        : unavailable(action == A::UNPIN ? "These folders are not pinned." : "These folders are already pinned.");
  case A::PIN_CURRENT:
  case A::UNPIN_CURRENT:
    if (!r.folder || r.folder->isTrash())
      return unavailable("This location cannot be pinned.");
    if (!c.pinsEditable)
      return unavailable("Reload valid pins before editing them, or wait for the save to finish.");
    return c.pinned && c.pinned(*r.folder) == (action == A::UNPIN) ? ActionState{}
                                                                   : unavailable("The pin state changed.");
  case A::RELOAD_PINS:
    return c.pinsBusy ? unavailable("Pins are being saved.") : ActionState{};
  case A::EMPTY_TRASH:
    return c.canEmptyTrash ? ActionState{}
                           : unavailable("Trash is unavailable or another operation is still finishing.");
  case A::SELECT_ALL:
  case A::INVERT_SELECTION:
  case A::SELECT_PATTERN:
    return c.model.rows().empty() ? unavailable("There are no visible items.") : ActionState{};
  case A::GRID:
    return {.reason = {}, .checked = c.viewMode == ViewMode::GRID ? 1 : 0};
  case A::LIST:
    return {.reason = {}, .checked = c.viewMode == ViewMode::LIST ? 1 : 0};
  case A::SHOW_HIDDEN:
    return {.reason = {}, .checked = c.model.filter().showHidden ? 1 : 0};
  case A::SHOW_SIDEBAR:
    return {.reason = {}, .checked = c.showSidebar ? 1 : 0};
  case A::SORT_NAME:
    return {.reason = {}, .checked = c.model.sort().field == SortField::NAME ? 1 : 0};
  case A::SORT_TYPE:
    return {.reason = {}, .checked = c.model.sort().field == SortField::TYPE ? 1 : 0};
  case A::SORT_SIZE:
    return {.reason = {}, .checked = c.model.sort().field == SortField::SIZE ? 1 : 0};
  case A::SORT_MODIFIED:
    return {.reason = {}, .checked = c.model.sort().field == SortField::MODIFIED ? 1 : 0};
  case A::ASCENDING:
    return {.reason = {}, .checked = c.model.sort().descending ? 0 : 1};
  case A::DESCENDING:
    return {.reason = {}, .checked = c.model.sort().descending ? 1 : 0};
  case A::FOLDERS_FIRST:
    return {.reason = {}, .checked = c.model.sort().directoriesFirst ? 1 : 0};
  case A::SINGLE_CLICK:
    return {.reason = {}, .checked = c.activationMode == ActivationMode::SINGLE_CLICK ? 1 : 0};
  case A::DOUBLE_CLICK:
    return {.reason = {}, .checked = c.activationMode == ActivationMode::DOUBLE_CLICK ? 1 : 0};
  case A::TYPE_TO_SEARCH:
    return {.reason = {}, .checked = c.typingMode == TypingMode::SEARCH ? 1 : 0};
  case A::TYPEAHEAD:
    return {.reason = {}, .checked = c.typingMode == TypingMode::TYPEAHEAD ? 1 : 0};
  case A::HOME:
  case A::ROOT:
  case A::EDIT_LOCATION:
  case A::PROPERTIES:
  case A::CLEAR_SELECTION:
  case A::VIEW_MENU:
  case A::SORT_MENU:
  case A::SELECTION_MENU:
  case A::INTERACTION_MENU:
  case A::SETTINGS:
  case A::ABOUT:
    return {};
  }
  return unavailable("Unknown action.");
}
