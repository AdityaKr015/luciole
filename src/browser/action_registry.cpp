#include "browser/action_registry.h"

#include "fs/archive_type.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace {
  using A = files::BrowserAction;
  using S = files::ActionScope;
  constexpr auto kActions = std::to_array<files::ActionDefinition>(
      {{A::Back, "Back", "Alt+Left"},
       {A::Forward, "Forward", "Alt+Right"},
       {A::Up, "Parent folder", "Alt+Up"},
       {A::Home, "Home", "Alt+Home"},
       {A::Root, "File System", ""},
       {A::Refresh, "Refresh", "F5"},
       {A::Stop, "Stop loading", ""},
       {A::EditLocation, "Edit location", "Ctrl+L"},
       {A::Search, "Search files", "Ctrl+F"},
       {A::Grid, "Grid view", "Ctrl+1"},
       {A::List, "List view", "Ctrl+2"},
       {A::Miller, "Column view", "Ctrl+3"},
       {A::ShowHidden, "Show hidden files", "Ctrl+H"},
       {A::ShowSidebar, "Show sidebar", "F9"},
       {A::ShowPreview, "Show preview", "F8"},
       {A::QuickPreview, "Preview", "Space", S::Targets},
       {A::OpenWith, "Open With…", "", S::Targets},
       {A::SortName, "Name", ""},
       {A::SortType, "Type", ""},
       {A::SortSize, "Size", ""},
       {A::SortModified, "Modified", ""},
       {A::Ascending, "Ascending", ""},
       {A::Descending, "Descending", ""},
       {A::FoldersFirst, "Folders first", ""},
       {A::Open, "Open", "Enter", S::Targets},
       {A::ContainingFolder, "Show in containing folder", "", S::Targets},
       {A::OriginalLocation, "Show original location", "", S::Targets},
       {A::Pin, "Pin to sidebar", "", S::Targets},
       {A::Unpin, "Unpin", "", S::Targets},
       {A::CutFiles, "Cut", "Ctrl+X", S::Targets},
       {A::CopyFiles, "Copy", "Ctrl+C", S::Targets},
       {A::PasteFiles, "Paste", "Ctrl+V", S::Location},
       {A::Rename, "Rename…", "F2", S::Targets},
       {A::CreateLink, "Create Link…", "", S::Targets},
       {A::Trash, "Move to Trash", "Delete", S::Targets},
       {A::Restore, "Restore", "", S::Targets},
       {A::DeletePermanently, "Delete Permanently…", "Delete", S::Targets, true},
       {A::Properties, "Properties", "Alt+Return", S::Targets},
       {A::Compress, "Compress…", "", S::Targets},
       {A::ExtractHere, "Extract Here", "", S::Targets},
       {A::ExtractTo, "Extract To…", "", S::Targets},
       {A::BrowseArchive, "Browse Archive", "", S::Targets},
       {A::CreateFolder, "New Folder…", "Ctrl+Shift+N", S::Location},
       {A::CreateFile, "Empty File…", "", S::Location},
       {A::NewDocument, "New Document", "", S::Location},
       {A::PinCurrent, "Pin this folder to sidebar", "", S::Location},
       {A::UnpinCurrent, "Unpin this folder", "", S::Location},
       {A::ReloadPins, "Reload pins", ""},
       {A::EmptyTrash, "Empty Trash…", "", S::Global, true},
       {A::SelectAll, "Select all", "Ctrl+A", S::Selection},
       {A::InvertSelection, "Invert selection", "Ctrl+Shift+I", S::Selection},
       {A::SelectPattern, "Select by pattern…", "Ctrl+S", S::Selection},
       {A::ClearSelection, "Clear selection", "Escape", S::Selection},
       {A::SingleClick, "Single click", ""},
       {A::DoubleClick, "Double click", ""},
       {A::TypeToSearch, "Search subfolders", ""},
       {A::Typeahead, "Select by typing", ""},
       {A::Settings, "Settings…", "Ctrl+,"},
       {A::About, "About Noctalia Files", ""},
       {A::ViewMenu, "Back", ""},
       {A::SortMenu, "Sort…", ""},
       {A::SelectionMenu, "Selection…", ""},
       {A::InteractionMenu, "Interaction…", ""},
       {A::CopyPath, "Copy folder path", "", S::Location},
       {A::CopyUri, "Copy folder URI", "", S::Location},
       {A::Terminal, "Open Terminal Here", "", S::Location},
       {A::CopyTargetPath, "Copy path", "", S::Targets},
       {A::CopyTargetUri, "Copy URI", "", S::Targets},
       {A::TargetTerminal, "Open in Terminal", "", S::Targets}}
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
  if (scope == ActionScope::Global) {
    return true;
  }
  if (request.context != context.context
      || request.folder != context.model.snapshot().location
      || request.query != context.query) {
    return false;
  }
  return scope != ActionScope::Targets || std::ranges::all_of(request.targets, [&](const auto& target) {
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
  if (scope == ActionScope::Targets && r.targets.empty()) {
    return unavailable("Select an item first.");
  }
  if ((scope == ActionScope::Targets || scope == ActionScope::Selection) && !c.model.selectionAvailable()) {
    return unavailable("The listing is not available for selection.");
  }
  const bool ready = snapshot.status == DirectoryStatus::Ready || snapshot.status == DirectoryStatus::Loading;
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
  case A::CreateLink:
    if (c.operationsBusy) {
      return unavailable("Another file operation is still finishing.");
    }
    return single() && all([](const auto& entry) {
             return entry.location.nativePath().has_value()
                 && !entry.location.isTrash()
                 && entry.identity.has_value()
                 && (entry.type == G_FILE_TYPE_REGULAR
                     || entry.type == G_FILE_TYPE_DIRECTORY
                     || entry.type == G_FILE_TYPE_SYMBOLIC_LINK);
           })
        ? ActionState{}
        : unavailable("Select one native file, folder or symbolic link.");
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
  case A::Terminal:
  case A::TargetTerminal:
    if (!c.terminalAvailable) {
      return unavailable("Install xdg-terminal-exec to open a terminal.");
    }
    if (action == A::Terminal) {
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
  case A::Back:
    return c.canBack ? ActionState{} : unavailable("There is no previous folder.");
  case A::Forward:
    return c.canForward ? ActionState{} : unavailable("There is no next folder.");
  case A::Up:
    return c.canUp ? ActionState{} : unavailable("This location has no parent folder.");
  case A::Refresh:
  case A::Search:
    return snapshot.location ? ActionState{} : unavailable("Open a folder first.");
  case A::Stop:
    return snapshot.status == DirectoryStatus::Loading ? ActionState{} : unavailable("Nothing is loading.");
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
  case A::NewDocument:
    if (!native || !c.query.empty() || !ready) {
      return unavailable("Open an available native folder first.");
    }
    if (snapshot.metadata.access.write == false) {
      return unavailable("Writing to this folder is not permitted.");
    }
    return writable();
  case A::Restore:
  case A::DeletePermanently:
    if (const auto state = writable(); !state.enabled) {
      return state;
    }
    if (r.folder != Location::trash() || !c.query.empty() || !ready) {
      return unavailable("Select items directly in Trash first.");
    }
    if (!all([](const auto& entry) {
          const GioPtr<GFile> parent(g_file_get_parent(entry.location.file().get()));
          return entry.location.isTrash()
              && parent
              && g_file_equal(parent.get(), Location::trash().file().get())
              && entry.identity
              && entry.access.remove.value_or(true);
        })) {
      return unavailable("These items cannot be changed by the Trash provider.");
    }
    if (action == A::Restore && !all([](const auto& entry) {
          return entry.trash.original && entry.trash.original->nativePath().has_value();
        })) {
      return unavailable("The original location is unavailable for one or more selected items.");
    }
    return {};
  case A::Rename:
  case A::Trash: {
    if (const auto state = writable(); !state.enabled) {
      return state;
    }
    if (action == A::Rename && !single()) {
      return unavailable("Select exactly one item to rename.");
    }
    if (!all([](const auto& entry) { return entry.location.nativePath().has_value(); })) {
      return unavailable("This operation currently supports native files only.");
    }
    if (action == A::Trash && (!native || !c.query.empty() || !ready)) {
      return unavailable("Open the containing native folder before moving items to Trash.");
    }
    if (!all([&](const auto& entry) {
          return (action == A::Rename ? entry.access.rename : entry.access.trash).value_or(true);
        })) {
      return unavailable("The provider does not permit this operation on every selected item.");
    }
    return {};
  }
  case A::Compress:
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
    if (action == A::Compress) {
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
  case A::Open:
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
  case A::Pin:
  case A::Unpin:
    if (!all([](const auto& entry) { return entry.type == G_FILE_TYPE_DIRECTORY && !entry.location.isTrash(); })) {
      return unavailable("Only folders outside Trash can be pinned.");
    }
    if (!c.pinsEditable) {
      return unavailable("Reload valid pins before editing them, or wait for the save to finish.");
    }
    return anyPinned(action == A::Unpin)
        ? ActionState{}
        : unavailable(action == A::Unpin ? "These folders are not pinned." : "These folders are already pinned.");
  case A::PinCurrent:
  case A::UnpinCurrent:
    if (!r.folder || r.folder->isTrash()) {
      return unavailable("This location cannot be pinned.");
    }
    if (!c.pinsEditable) {
      return unavailable("Reload valid pins before editing them, or wait for the save to finish.");
    }
    return c.pinned && c.pinned(*r.folder) == (action == A::Unpin) ? ActionState{}
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
  case A::Grid:
    return {.reason = {}, .checked = c.viewMode == ViewMode::Grid ? 1 : 0};
  case A::Miller:
    return {.reason = {}, .checked = c.viewMode == ViewMode::Miller ? 1 : 0};
  case A::List:
    return {.reason = {}, .checked = c.viewMode == ViewMode::List ? 1 : 0};
  case A::ShowHidden:
    return {.reason = {}, .checked = c.model.filter().showHidden ? 1 : 0};
  case A::ShowSidebar:
    return {.reason = {}, .checked = c.showSidebar ? 1 : 0};
  case A::SortName:
    return {.reason = {}, .checked = c.model.sort().field == SortField::Name ? 1 : 0};
  case A::SortType:
    return {.reason = {}, .checked = c.model.sort().field == SortField::Type ? 1 : 0};
  case A::SortSize:
    return {.reason = {}, .checked = c.model.sort().field == SortField::Size ? 1 : 0};
  case A::SortModified:
    return {.reason = {}, .checked = c.model.sort().field == SortField::Modified ? 1 : 0};
  case A::Ascending:
    return {.reason = {}, .checked = c.model.sort().descending ? 0 : 1};
  case A::Descending:
    return {.reason = {}, .checked = c.model.sort().descending ? 1 : 0};
  case A::FoldersFirst:
    return {.reason = {}, .checked = c.model.sort().directoriesFirst ? 1 : 0};
  case A::SingleClick:
    return {.reason = {}, .checked = c.activationMode == ActivationMode::SingleClick ? 1 : 0};
  case A::DoubleClick:
    return {.reason = {}, .checked = c.activationMode == ActivationMode::DoubleClick ? 1 : 0};
  case A::TypeToSearch:
    return {.reason = {}, .checked = c.typingMode == TypingMode::Search ? 1 : 0};
  case A::Typeahead:
    return {.reason = {}, .checked = c.typingMode == TypingMode::Typeahead ? 1 : 0};
  case A::Home:
  case A::Root:
  case A::EditLocation:
  case A::Properties:
  case A::ClearSelection:
  case A::ViewMenu:
  case A::SortMenu:
  case A::SelectionMenu:
  case A::InteractionMenu:
  case A::Settings:
  case A::About:
    return {};
  }
  return unavailable("Unknown action.");
}
