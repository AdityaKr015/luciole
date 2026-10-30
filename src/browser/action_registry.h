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
    EDIT_LOCATION,
    SEARCH,
    GRID,
    LIST,
    SHOW_HIDDEN,
    SHOW_SIDEBAR,
    SORT_NAME,
    SORT_TYPE,
    SORT_SIZE,
    SORT_MODIFIED,
    ASCENDING,
    DESCENDING,
    FOLDERS_FIRST,
    OPEN,
    CONTAINING_FOLDER,
    ORIGINAL_LOCATION,
    PIN,
    UNPIN,
    RENAME,
    TRASH,
    PROPERTIES,
    CREATE_FOLDER,
    CREATE_FILE,
    PIN_CURRENT,
    UNPIN_CURRENT,
    RELOAD_PINS,
    EMPTY_TRASH,
    SELECT_ALL,
    INVERT_SELECTION,
    SELECT_PATTERN,
    CLEAR_SELECTION,
    SINGLE_CLICK,
    DOUBLE_CLICK,
    TYPE_TO_SEARCH,
    TYPEAHEAD,
    SETTINGS,
    ABOUT,
    VIEW_MENU,
    SORT_MENU,
    SELECTION_MENU,
    INTERACTION_MENU,
    COPY_PATH,
    COPY_URI,
    TERMINAL,
    COPY_TARGET_PATH,
    COPY_TARGET_URI,
    TARGET_TERMINAL
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
    bool clipboardAvailable = false;
    bool terminalAvailable = false;
    ActivationMode activationMode = ActivationMode::DOUBLE_CLICK;
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
