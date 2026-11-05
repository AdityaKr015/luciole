#pragma once

#include "browser/directory_model.h"

#include <functional>
#include <span>
#include <string_view>

namespace files {
  enum class BrowserAction : std::uint8_t {
    BACK,
    FORWARD,
    UP,
    HOME,
    ROOT,
    REFRESH,
    STOP,
    EditLocation,
    SEARCH,
    GRID,
    LIST,
    MILLER,
    ShowHidden,
    ShowSidebar,
    ShowPreview,
    QuickPreview,
    OpenWith,
    SortName,
    SortType,
    SortSize,
    SortModified,
    ASCENDING,
    DESCENDING,
    FoldersFirst,
    OPEN,
    ContainingFolder,
    OriginalLocation,
    PIN,
    UNPIN,
    CutFiles,
    CopyFiles,
    PasteFiles,
    RENAME,
    TRASH,
    PROPERTIES,
    COMPRESS,
    ExtractHere,
    ExtractTo,
    BrowseArchive,
    CreateFolder,
    CreateFile,
    PinCurrent,
    UnpinCurrent,
    ReloadPins,
    EmptyTrash,
    SelectAll,
    InvertSelection,
    SelectPattern,
    ClearSelection,
    SingleClick,
    DoubleClick,
    TypeToSearch,
    TYPEAHEAD,
    SETTINGS,
    ABOUT,
    ViewMenu,
    SortMenu,
    SelectionMenu,
    InteractionMenu,
    CopyPath,
    CopyUri,
    TERMINAL,
    CopyTargetPath,
    CopyTargetUri,
    TargetTerminal
  };
  enum class ActionScope : std::uint8_t { GLOBAL, LOCATION, TARGETS, SELECTION };
  struct ActionDefinition {
    BrowserAction action;
    std::string_view label;
    std::string_view shortcut;
    ActionScope scope = ActionScope::GLOBAL;
    bool destructive = false;
  };
  struct ActionTarget {
    EntryId id;
    Location location;
  };
  // Menus retain identity and context, never a borrowed row or operation index.
  struct ActionRequest {
    std::uint64_t context = 0;
    std::optional<Location> folder;
    std::string query;
    std::vector<ActionTarget> targets;
  };
  struct ActionContext {
    const DirectoryModel& model;
    std::uint64_t context = 0;
    std::string_view query;
    bool canBack = false, canForward = false, canUp = false;
    bool operationsBusy = false;
    bool pinsEditable = false, pinsBusy = false, canEmptyTrash = false;
    std::function<bool(const Location&)> pinned;
    ViewMode viewMode = ViewMode::LIST;
    bool showSidebar = true;
    bool showPreview = false;
    bool clipboardAvailable = false;
    bool clipboardHasFiles = false;
    bool terminalAvailable = false;
    ActivationMode activationMode = ActivationMode::DoubleClick;
    TypingMode typingMode = TypingMode::SEARCH;
  };
  struct ActionState {
    bool enabled = true;
    std::string_view reason;
    int checked = -1;
  };
  class ActionRegistry {
  public:
    static std::span<const ActionDefinition> definitions();
    static const ActionDefinition& definition(BrowserAction action);
    static ActionRequest capture(const ActionContext& context);
    static bool current(const ActionContext& context, const ActionRequest& request, ActionScope scope);
    static ActionState state(BrowserAction action, const ActionContext& context, const ActionRequest& request);
  };
} // namespace files
