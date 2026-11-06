#pragma once

#include "browser/directory_model.h"

#include <functional>
#include <span>
#include <string_view>

namespace files {
  enum class BrowserAction : std::uint8_t {
    Back,
    Forward,
    Up,
    Home,
    Root,
    Refresh,
    Stop,
    EditLocation,
    Search,
    Grid,
    List,
    Miller,
    ShowHidden,
    ShowSidebar,
    ShowPreview,
    QuickPreview,
    OpenWith,
    SortName,
    SortType,
    SortSize,
    SortModified,
    Ascending,
    Descending,
    FoldersFirst,
    Open,
    ContainingFolder,
    OriginalLocation,
    Pin,
    Unpin,
    CutFiles,
    CopyFiles,
    PasteFiles,
    Rename,
    CreateLink,
    Trash,
    Restore,
    DeletePermanently,
    Properties,
    Compress,
    ExtractHere,
    ExtractTo,
    BrowseArchive,
    CreateFolder,
    CreateFile,
    NewDocument,
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
    Typeahead,
    Settings,
    About,
    ViewMenu,
    SortMenu,
    SelectionMenu,
    InteractionMenu,
    CopyPath,
    CopyUri,
    Terminal,
    CopyTargetPath,
    CopyTargetUri,
    TargetTerminal
  };
  enum class ActionScope : std::uint8_t { Global, Location, Targets, Selection };
  struct ActionDefinition {
    BrowserAction action;
    std::string_view label;
    std::string_view shortcut;
    ActionScope scope = ActionScope::Global;
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
    ViewMode viewMode = ViewMode::List;
    bool showSidebar = true;
    bool showPreview = false;
    bool clipboardAvailable = false;
    bool clipboardHasFiles = false;
    bool terminalAvailable = false;
    ActivationMode activationMode = ActivationMode::DoubleClick;
    TypingMode typingMode = TypingMode::Search;
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
