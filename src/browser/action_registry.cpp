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
       {A::EditLocation, "Edit location", "Ctrl+L"},
       {A::SEARCH, "Search files", "Ctrl+F"},
       {A::GRID, "Grid view", "Ctrl+1"},
       {A::LIST, "List view", "Ctrl+2"},
       {A::MILLER, "Column view", "Ctrl+3"},
       {A::ShowHidden, "Show hidden files", "Ctrl+H"},
       {A::ShowSidebar, "Show sidebar", "F9"},
       {A::ShowPreview, "Show preview", "F8"},
       {A::QuickPreview, "Preview", "Space", S::TARGETS},
       {A::OpenWith, "Open With…", "", S::TARGETS},
       {A::SortName, "Name", ""},
       {A::SortType, "Type", ""},
       {A::SortSize, "Size", ""},
       {A::SortModified, "Modified", ""},
       {A::ASCENDING, "Ascending", ""},
       {A::DESCENDING, "Descending", ""},
       {A::FoldersFirst, "Folders first", ""},
       {A::OPEN, "Open", "Enter", S::TARGETS},
       {A::ContainingFolder, "Show in containing folder", "", S::TARGETS},
       {A::OriginalLocation, "Show original location", "", S::TARGETS},
       {A::PIN, "Pin to sidebar", "", S::TARGETS},
       {A::UNPIN, "Unpin", "", S::TARGETS},
       {A::CutFiles, "Cut", "Ctrl+X", S::TARGETS},
       {A::CopyFiles, "Copy", "Ctrl+C", S::TARGETS},
       {A::PasteFiles, "Paste", "Ctrl+V", S::LOCATION},
       {A::RENAME, "Rename…", "F2", S::TARGETS},
       {A::TRASH, "Move to Trash", "Delete", S::TARGETS},
       {A::PROPERTIES, "Properties", "Alt+Return", S::TARGETS},
       {A::COMPRESS, "Compress…", "", S::TARGETS},
       {A::ExtractHere, "Extract Here", "", S::TARGETS},
       {A::ExtractTo, "Extract To…", "", S::TARGETS},
       {A::BrowseArchive, "Browse Archive", "", S::TARGETS},
       {A::CreateFolder, "New Folder…", "Ctrl+Shift+N", S::LOCATION},
       {A::CreateFile, "Empty File…", "", S::LOCATION},
       {A::PinCurrent, "Pin this folder to sidebar", "", S::LOCATION},
       {A::UnpinCurrent, "Unpin this folder", "", S::LOCATION},
       {A::ReloadPins, "Reload pins", ""},
       {A::EmptyTrash, "Empty Trash…", "", S::GLOBAL, true},
       {A::SelectAll, "Select all", "Ctrl+A", S::SELECTION},
       {A::InvertSelection, "Invert selection", "Ctrl+Shift+I", S::SELECTION},
       {A::SelectPattern, "Select by pattern…", "Ctrl+S", S::SELECTION},
       {A::ClearSelection, "Clear selection", "Escape", S::SELECTION},
       {A::SingleClick, "Single click", ""},
       {A::DoubleClick, "Double click", ""},
       {A::TypeToSearch, "Search subfolders", ""},
       {A::TYPEAHEAD, "Select by typing", ""},
       {A::SETTINGS, "Settings…", "Ctrl+,"},
       {A::ABOUT, "About Noctalia Files", ""},
       {A::ViewMenu, "Back", ""},
       {A::SortMenu, "Sort…", ""},
       {A::SelectionMenu, "Selection…", ""},
       {A::InteractionMenu, "Interaction…", ""},
       {A::CopyPath, "Copy folder path", "", S::LOCATION},
       {A::CopyUri, "Copy folder URI", "", S::LOCATION},
       {A::TERMINAL, "Open Terminal Here", "", S::LOCATION},
       {A::CopyTargetPath, "Copy path", "", S::TARGETS},
       {A::CopyTargetUri, "Copy URI", "", S::TARGETS},
       {A::TargetTerminal, "Open in Terminal", "", S::TARGETS}}
  );
  files::ActionState unavailable(std::string_view reason) { return {.enabled = false, .reason = reason}; }
} // namespace

std::span<const files::ActionDefinition> files::ActionRegistry::definitions() { return kActions; }
const files::ActionDefinition& files::ActionRegistry::definition(BrowserAction action) {
  const auto* found = std::ranges::find(kActions, action, &ActionDefinition::action);
  if (found == kActions.end()) {
    throw std::invalid_argument("unknown browser action");
  }
  return *found;
}
files::ActionRequest files::ActionRegistry::capture(const ActionContext& context) {
  ActionRequest request{context.context, context.model.snapshot().location, std::string(context.query), {}};
  for (const auto id : context.model.selectedIds()) {
    if (const auto* entry = context.model.entry(id)) {
      request.targets.push_back({id, entry->location});
    }
  }
  return request;
}
bool files::ActionRegistry::current(const ActionContext& context, const ActionRequest& request, ActionScope scope) {
  if (scope == ActionScope::GLOBAL) {
    return true;
  }
  if (request.context != context.context
      || request.folder != context.model.snapshot().location
      || request.query != context.query) {
    return false;
  }
  return scope != ActionScope::TARGETS || std::ranges::all_of(request.targets, [&](const auto& target) {
           const auto* entry = context.model.entry(target.id);
           return entry != nullptr
               && entry->location == target.location
               && std::ranges::contains(context.model.rows(), target.id);
         });
}
files::ActionState files::ActionRegistry::state(BrowserAction action, const ActionContext& c, const ActionRequest& r) {
  const auto scope = definition(action).scope;
  if (!current(c, r, scope)) {
    return unavailable("The folder or targets changed. Open the menu again.");
  }
  const auto& snapshot = c.model.snapshot();
  if (scope == ActionScope::TARGETS && r.targets.empty()) {
    return unavailable("Select an item first.");
  }
  if ((scope == ActionScope::TARGETS || scope == ActionScope::SELECTION) && !c.model.selectionAvailable()) {
    return unavailable("The listing is not available for selection.");
  }
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
  const auto writable = [&] -> ActionState {
    if (c.operationsBusy) {
      return unavailable("Another file operation is still finishing.");
    }
    if (snapshot.metadata.filesystem.readOnly == true) {
      return unavailable("The filesystem is read-only.");
    }
    return {};
  };
  switch (action) {
  case A::ShowPreview:
    return {.enabled = true, .reason = {}, .checked = c.showPreview ? 1 : 0};
  case A::QuickPreview:
    return single() ? ActionState{} : unavailable("Select one item to preview.");
  case A::OpenWith:
    return single() && all([](const DirectoryEntry& entry) {
             return entry.type == G_FILE_TYPE_REGULAR && !entry.symlink && entry.access.read != false;
           })
        ? ActionState{}
        : unavailable("Select one readable file.");
  case A::CopyPath:
  case A::CopyUri:
    if (!r.folder) {
      return unavailable("Open a folder first.");
    }
    [[fallthrough]];
  case A::CopyTargetPath:
  case A::CopyTargetUri:
    return c.clipboardAvailable ? ActionState{} : unavailable("The application clipboard is unavailable.");
  case A::TERMINAL:
  case A::TargetTerminal:
    if (!c.terminalAvailable) {
      return unavailable("Install xdg-terminal-exec to open a terminal.");
    }
    if (action == A::TERMINAL) {
      if (!native || r.folder->isTrash()) {
        return unavailable("Terminals require a native folder.");
      }
      if (snapshot.metadata.access.execute == false) {
        return unavailable("Access to this folder is not permitted.");
      }
    } else {
      if (!single() || !all([](const auto& entry) {
            return entry.type == G_FILE_TYPE_DIRECTORY
                && entry.location.nativePath().has_value()
                && entry.access.execute.value_or(true);
          })) {
        return unavailable("Select one accessible native folder.");
      }
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
  case A::CutFiles:
  case A::CopyFiles:
    if (!c.clipboardAvailable) {
      return unavailable("The application clipboard is unavailable.");
    }
    if (!all([&](const auto& entry) {
          return entry.location.nativePath().has_value()
              && entry.access.read.value_or(true)
              && (entry.type == G_FILE_TYPE_DIRECTORY || entry.type == G_FILE_TYPE_REGULAR || entry.symlink)
              && (action != A::CutFiles || entry.access.rename.value_or(true));
        })) {
      return unavailable("Select accessible native files or folders outside Trash.");
    }
    return {};
  case A::PasteFiles:
    if (!c.clipboardAvailable || !c.clipboardHasFiles) {
      return unavailable("The clipboard has no available files.");
    }
    [[fallthrough]];
  case A::CreateFolder:
  case A::CreateFile:
    if (!native || !c.query.empty() || !ready) {
      return unavailable("Open an available native folder first.");
    }
    if (snapshot.metadata.access.write == false) {
      return unavailable("Writing to this folder is not permitted.");
    }
    return writable();
  case A::RENAME:
  case A::TRASH: {
    if (const auto state = writable(); !state.enabled) {
      return state;
    }
    if (action == A::RENAME && !single()) {
      return unavailable("Select exactly one item to rename.");
    }
    if (!all([](const auto& entry) { return entry.location.nativePath().has_value(); })) {
      return unavailable("This operation currently supports native files only.");
    }
    if (action == A::TRASH && (!native || !c.query.empty() || !ready)) {
      return unavailable("Open the containing native folder before moving items to Trash.");
    }
    if (!all([&](const auto& entry) {
          return (action == A::RENAME ? entry.access.rename : entry.access.trash).value_or(true);
        })) {
      return unavailable("The provider does not permit this operation on every selected item.");
    }
    return {};
  }
  case A::COMPRESS:
  case A::ExtractHere:
  case A::ExtractTo:
  case A::BrowseArchive:
    if (c.operationsBusy) {
      return unavailable("Another file operation is still finishing.");
    }
    if (!all([](const auto& entry) {
          return entry.location.nativePath().has_value() && entry.access.read.value_or(true);
        })) {
      return unavailable("Archives require readable native items.");
    }
    if (action == A::COMPRESS) {
      if (!all([](const auto& entry) {
            return entry.type == G_FILE_TYPE_REGULAR || entry.type == G_FILE_TYPE_DIRECTORY || entry.symlink;
          })) {
        return unavailable("Special files cannot be compressed.");
      }
    } else if (!single() || !all([](const auto& entry) {
                 return entry.type == G_FILE_TYPE_REGULAR
                     && !entry.symlink
                     && isSupportedArchive(entry.location, entry.contentType);
               })) {
      return unavailable("Select one ZIP or TAR archive.");
    }
    return {};
  case A::OPEN:
    if (!single() && !all([](const auto& entry) { return entry.type == G_FILE_TYPE_REGULAR; })) {
      return unavailable("Open folders individually.");
    }
    if (!all([](const auto& entry) {
          return !entry.location.isTrash() || (entry.type == G_FILE_TYPE_DIRECTORY && !entry.symlink);
        })) {
      return unavailable("Restore this item before opening it.");
    }
    if (!all([](const auto& entry) { return entry.access.read.value_or(true); })) {
      return unavailable("Reading this item is not permitted.");
    }
    return {};
  case A::ContainingFolder:
  case A::OriginalLocation:
    if (!single()) {
      return unavailable("Select exactly one item.");
    }
    if (action == A::OriginalLocation && !c.model.entry(r.targets.front().id)->trash.original) {
      return unavailable("The original location is unavailable.");
    }
    return {};
  case A::PIN:
  case A::UNPIN:
    if (!all([](const auto& entry) { return entry.type == G_FILE_TYPE_DIRECTORY && !entry.location.isTrash(); })) {
      return unavailable("Only folders outside Trash can be pinned.");
    }
    if (!c.pinsEditable) {
      return unavailable("Reload valid pins before editing them, or wait for the save to finish.");
    }
    return anyPinned(action == A::UNPIN)
        ? ActionState{}
        : unavailable(action == A::UNPIN ? "These folders are not pinned." : "These folders are already pinned.");
  case A::PinCurrent:
  case A::UnpinCurrent:
    if (!r.folder || r.folder->isTrash()) {
      return unavailable("This location cannot be pinned.");
    }
    if (!c.pinsEditable) {
      return unavailable("Reload valid pins before editing them, or wait for the save to finish.");
    }
    return c.pinned && c.pinned(*r.folder) == (action == A::UNPIN) ? ActionState{}
                                                                   : unavailable("The pin state changed.");
  case A::ReloadPins:
    return c.pinsBusy ? unavailable("Pins are being saved.") : ActionState{};
  case A::EmptyTrash:
    return c.canEmptyTrash ? ActionState{}
                           : unavailable("Trash is unavailable or another operation is still finishing.");
  case A::SelectAll:
  case A::InvertSelection:
  case A::SelectPattern:
    return c.model.rows().empty() ? unavailable("There are no visible items.") : ActionState{};
  case A::GRID:
    return {.reason = {}, .checked = c.viewMode == ViewMode::GRID ? 1 : 0};
  case A::MILLER:
    return {.reason = {}, .checked = c.viewMode == ViewMode::MILLER ? 1 : 0};
  case A::LIST:
    return {.reason = {}, .checked = c.viewMode == ViewMode::LIST ? 1 : 0};
  case A::ShowHidden:
    return {.reason = {}, .checked = c.model.filter().showHidden ? 1 : 0};
  case A::ShowSidebar:
    return {.reason = {}, .checked = c.showSidebar ? 1 : 0};
  case A::SortName:
    return {.reason = {}, .checked = c.model.sort().field == SortField::NAME ? 1 : 0};
  case A::SortType:
    return {.reason = {}, .checked = c.model.sort().field == SortField::TYPE ? 1 : 0};
  case A::SortSize:
    return {.reason = {}, .checked = c.model.sort().field == SortField::SIZE ? 1 : 0};
  case A::SortModified:
    return {.reason = {}, .checked = c.model.sort().field == SortField::MODIFIED ? 1 : 0};
  case A::ASCENDING:
    return {.reason = {}, .checked = c.model.sort().descending ? 0 : 1};
  case A::DESCENDING:
    return {.reason = {}, .checked = c.model.sort().descending ? 1 : 0};
  case A::FoldersFirst:
    return {.reason = {}, .checked = c.model.sort().directoriesFirst ? 1 : 0};
  case A::SingleClick:
    return {.reason = {}, .checked = c.activationMode == ActivationMode::SingleClick ? 1 : 0};
  case A::DoubleClick:
    return {.reason = {}, .checked = c.activationMode == ActivationMode::DoubleClick ? 1 : 0};
  case A::TypeToSearch:
    return {.reason = {}, .checked = c.typingMode == TypingMode::SEARCH ? 1 : 0};
  case A::TYPEAHEAD:
    return {.reason = {}, .checked = c.typingMode == TypingMode::TYPEAHEAD ? 1 : 0};
  case A::HOME:
  case A::ROOT:
  case A::EditLocation:
  case A::PROPERTIES:
  case A::ClearSelection:
  case A::ViewMenu:
  case A::SortMenu:
  case A::SelectionMenu:
  case A::InteractionMenu:
  case A::SETTINGS:
  case A::ABOUT:
    return {};
  }
  return unavailable("Unknown action.");
}
