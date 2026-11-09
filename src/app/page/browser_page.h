#pragma once

#include "app/workspace_command.h"
#include "app/workspace_layout.h"
#include "browser/action_registry.h"
#include "browser/browser_controller.h"
#include "browser/path_completion.h"
#include "browser/selection_tools.h"
#include "browser/tab_workspace.h"
#include "clipboard/text_clipboard.h"
#include "config/settings.h"
#include "config/view_preference_writer.h"
#include "core/timer_manager.h"
#include "model/details_columns.h"
#include "operations/archive_service.h"
#include "operations/batch_rename_service.h"
#include "operations/create_service.h"
#include "operations/empty_trash_service.h"
#include "operations/permission_service.h"
#include "operations/rename_service.h"
#include "operations/restore_service.h"
#include "operations/transfer_service.h"
#include "operations/trash_service.h"
#include "platform/desktop_integration.h"
#include "platform/file_launcher.h"
#include "platform/terminal_launcher.h"
#include "render/animation/animation_manager.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "thumbnails/preview_service.h"
#include "ui/browser/file_icon.h"
#include "ui/browser/file_view.h"
#include "ui/browser/places_sidebar.h"
#include "ui/controls/acl_editor.h"
#include "ui/controls/archive_view.h"
#include "ui/controls/batch_rename_dialog.h"
#include "ui/controls/browser_settings_view.h"
#include "ui/controls/bulk_acl_editor.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/file_preview_view.h"
#include "ui/controls/input.h"
#include "ui/controls/link_dialog.h"
#include "ui/controls/miller_view.h"
#include "ui/controls/name_editor.h"
#include "ui/controls/permission_editor.h"
#include "ui/controls/permission_results_view.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select_popup_context.h"
#include "ui/controls/separator.h"
#include "ui/controls/tooltip_presenter.h"
#include "wayland/popup_surface.h"
#include "wayland/text_input_service.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

class BrowserPage : public TextInputClient, public SelectPopupContext {
public:
  using Action = files::BrowserAction;

  BrowserPage(
      WaylandConnection& connection, RenderContext& renderer, TextInputService& textInput,
      const files::BrowserSettings& settings, files::WaylandTextClipboard& clipboard, files::WindowSurface& surface,
      AnimationManager& animations, files::PinService& pins, files::FileIconCache& icons,
      files::ViewPreferenceWriter& preferences
  );

  files::BrowserController& browser();
  const files::BrowserController& browser() const;
  void focusMiller(std::size_t index, bool reveal = true);

  ~BrowserPage() override;

  void iconThemeChanged();

  bool initialize(const std::optional<std::string>& initialLocation);

  // Select `items` once the current folder finishes loading; optionally open their Properties.
  void revealItems(std::vector<files::Location> items, bool properties);

  bool renameBusy() const;
  bool busy() const;
  void shutdown();

  files::BrowserSettings options() const;
  std::optional<files::Location> location() const;
  std::string title() const;
  void preferencesChanged(const std::string& error);
  void placesChanged();
  void clipboardChanged();
  void clearExternalDrop();
  const std::vector<files::Bookmark>* draggedFolders() const;
  files::FileDropTarget externalDrop(float x, float y, const std::vector<files::Bookmark>* pins);
  files::FileDropTarget externalDestination(const files::Location& destination, LayoutRect rect);
  void iconsReady();
  void configurePlacesCallbacks();
  void reusePlacesFrom(BrowserPage& previous);
  void workspaceError(const std::string& message);
  void setWorkspaceId(files::TabId id);
  bool owns(const Node* node) const;
  void focusArea(InputArea* area);
  void leavePointer();
  InputArea* cycleFocus(bool reverse);
  bool completeLocationOnTab(std::uint32_t modifiers);
  void nativeFocus();
  void stampFocusKeys(Node& node);
  void duplicateFrom(BrowserPage& source);
  void open(const files::Location& location);
  void setWorkspaceRequest(std::function<void(WorkspaceCommand, std::optional<files::Location>)> callback);
  void setClearThumbnails(std::function<void()> callback);
  void clearThumbnails();
  bool clearingThumbnails() const;
  void pauseThumbnails(bool pause);
  void configurationChanged();
  void setHasOther(std::function<bool()> callback);
  void setPopupAvailable(std::function<bool()> callback);
  void setForeignDrop(std::function<std::optional<std::pair<files::Location, LayoutRect>>(float, float)> callback);
  bool popupOwns(wl_surface* surface) const;
  bool popup() const;
  bool overlay() const;
  bool previewVisible() const;
  int previewWidth() const;
  float previewAreaWidth() const;
  void savePreviewWidth(int width);
  void setPreviewBounds(std::optional<LayoutRect> bounds);
  void layoutNavigation(Renderer& renderer);
  bool dragging() const;
  bool pointerCaptured() const;
  bool contains(float x, float y) const;
  files::WindowSurface* boundSurface() const;
  void rebind(files::WindowSurface& surface);
  void prepare(LayoutRect bounds, bool active, Node* host, bool roundLeft = true, bool roundRight = true);
  void deactivate();
  void hide();
  void mount(Node& host);
  void unmount();
  Node* root() const;
  std::unique_ptr<Node> takeToolbar();
  void returnToolbar(std::unique_ptr<Node> node);
  void takeSidebar(Node& host);
  void returnSidebar();
  Node* sidebar() const;
  void ensureScene();

  float sidebarWidth() const;
  float sidebarControlWidth() const;
  void pumpJobs();
  void workspaceMenu(std::vector<ContextMenuControlEntry> entries, std::function<void(int)> callback, float x, float y);
  void transferTo(const files::Location& destination, bool copy);
  std::optional<std::pair<files::Location, LayoutRect>> foreignTarget(float x, float y) const;

  void outputChanged(wl_output* output);

  void pointer(const PointerEvent& event);

  void keyboard(const KeyboardEvent& event);

private:
  struct EntryTarget {
    files::EntryId id;
    files::Location location;
  };
  struct Drag {
    std::vector<EntryTarget> entries;
    std::vector<files::Bookmark> bookmarks;
    std::optional<files::Location> sourcePin;
    bool foldersOnly = false;
  };
  struct DropTarget {
    files::Location location;
    LayoutRect rect;
    std::optional<files::EntryId> entry = std::nullopt;
  };

  files::ActionContext actionContext() const;
  files::ActionRequest actionRequest(bool withTargets = true) const;
  files::ActionRequest requestFor(const std::vector<files::DirectoryEntry>& entries) const;
  files::ActionState actionState(Action action, const files::ActionRequest& request) const;
  ContextMenuControlEntry actionEntry(Action action, const files::ActionRequest& request, bool radio = false) const;
  void dispatchAction(Action action);
  void dispatchAction(Action action, const files::ActionRequest& request);
  void openPatternSelection(const files::ActionRequest& request);

  void actionError(std::string message);

  bool targetsCurrent(const std::vector<files::DirectoryEntry>& targets);

  void pinFolders(std::vector<files::Bookmark> bookmarks, std::size_t before);

  void openFileMenu(std::optional<files::EntryId> id, float x, float y);

  void openPlaceMenu(const files::Bookmark& bookmark, float x, float y);

  bool canEmptyTrash() const;
  void openEmptyTrash(std::optional<std::vector<files::DirectoryEntry>> selection = std::nullopt);
  void confirmEmptyTrash();
  void syncEmptyTrash();

  void updateOperationProgress();
  bool canTrashItems(const std::vector<files::DirectoryEntry>& targets) const;
  void trashItems(const std::vector<files::DirectoryEntry>& targets);
  void syncTrash();

  bool canCreateItem() const;
  void closeDocumentMenu();
  void dismissDocumentMenu();
  void fillDocumentMenu(const files::ActionRequest& request, files::TemplateListing listing, bool loading = false);
  void openDocumentMenu(const files::ActionRequest& request);
  void openCreateItem(
      files::Location parent, files::CreateKind kind, std::optional<files::DocumentTemplate> source = std::nullopt
  );
  void submitCreateItem(std::string name);
  void openLinkDialog(const files::DirectoryEntry& target);
  void syncCreation();

  void openBatchRename(const std::vector<files::DirectoryEntry>& targets);
  void syncBatchRename();

  bool canFileRename(const files::DirectoryEntry& entry) const;
  void openFileRename(files::DirectoryEntry target);
  void submitFileRename(std::string name);
  void syncFileRename();

  void openPlaceRename(const files::Bookmark& bookmark);

  void submitPlaceRename(std::string label);

  void syncPlaceRename();

  void showContaining(const files::Location& location);

  void applySettings(const files::BrowserSettings& settings, bool force);

  void showCenteredDialog(std::unique_ptr<Flex> root, float preferredWidth = 588.0F);

  static files::Location archiveDestination(const files::Location& source);
  void openArchiveDialog(std::vector<files::Location> sources, std::vector<std::string> selected, bool compress);
  void deferCloseArchive();
  void closeArchive();
  void syncArchive();

  void deferCloseSettings();
  void closeSettings();
  void openSettings();
  void openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) override;
  void closeSelectDropdown() override;
  bool isSelectDropdownOpen() const override;

  void openAbout();

  void syncPreview();
  void previewOpen(bool with);
  std::unique_ptr<FilePreviewView> makePreviewView(bool quick);
  void showQuickPreview();
  void showApplications(const files::DirectoryEntry& entry);
  void showAdvancedPermissions(bool advanced, bool focus = false);
  bool propertiesCanApply() const;
  void syncPermissions();
  void showProperties(const std::vector<files::DirectoryEntry>& targets, bool captureCurrentLocation = false);

  void syncTransferConflict();

  void retryTransfer();

  bool
  startPayloadTransfer(files::FilePayload payload, const files::Location& destination, files::TransferAction action);

  bool dragCurrent() const;

  bool validDropFolder(const files::Location& destination) const;

  std::optional<DropTarget> dropTarget() const;

  std::optional<std::size_t> pinDropSlot() const;

  void startFileDrag(const std::vector<files::EntryId>& ids);
  void startPlaceDrag(const files::Bookmark& bookmark);
  void cancelDrag();
  VirtualGridView* dragView() const;
  float dragScrollSpeed(bool& places) const;
  void updateExternalScroll();
  void updateDrag();
  void layoutDrag(Renderer& renderer);
  void finishDrag(float x, float y);
  void clearTypeahead();
  void typeaheadChanged(std::optional<files::EntryId> match, TextInputChangeCause cause = TextInputChangeCause::Other);
  void applyTypeahead(
      const std::string& text, TextInputChangeCause cause = TextInputChangeCause::Other, bool composing = false
  );
  TextInputState textInputState() const override;
  void textInputApplyEdit(const TextInputEdit& edit) override;
  void textInputResetPreedit() override;
  void textInputActivated(TextInputService&) override;
  void textInputDeactivated(TextInputService&) override;

  void clearCompletions();
  void requestCompletions(const std::string& text);
  void acceptCompletion(std::size_t index, bool navigate);
  bool completionKey(std::uint32_t key, std::uint32_t modifiers);
  void layoutCompletions(Renderer& renderer);
  void beginLocationEdit();
  void endLocationEdit(bool focusFiles);
  void breadcrumbClick(const files::Location& target, const InputArea::PointerData* pointer = nullptr);
  void openBreadcrumbMenu(const files::Location& target, LayoutRect anchor);
  void openBreadcrumbOverflow();
  void layoutBreadcrumbs(Renderer& renderer);
  void rebuildBreadcrumbs();

  void openFilter(bool selectAll = true, bool preserveTextInput = false);

  void changeNameFilter(const std::string& text);

  void closeFilter();

  static constexpr float kSidebarHandleWidth = 6.0F;
  static constexpr float kSidebarGapInset = (kWorkspaceInset - kSidebarHandleWidth) * 0.5F;

  void setSidebarVisible(bool visible, bool persist = true);

  int sidebarMaximumWidth() const;

  void resizeSidebar(int width, bool persist);

  void captureViewAnchor();

  void setDetailsColumnVisibility(files::DetailsColumnVisibility visibility);

  struct ColumnResize {
    std::size_t index;
    float pressX;
    float width;
    files::DetailsColumnWidths before;
    float offset;
    int nameWidth;
  };

  Button* widthHeader(std::size_t index) const;

  float headerWidth(std::size_t index) const;

  void cancelColumnResize();

  void setDetailsColumnWidths(files::DetailsColumnWidths widths);

  void scrollDetails(float offset);

  void layoutDetails(Renderer& renderer);

  struct ColumnDrag {
    files::DetailsColumn source;
    float pressX;
    float pressY;
    std::uint32_t cursor;
    bool active = false;
  };
  struct ColumnDrop {
    files::DetailsColumnOrder order;
    LayoutRect marker;
  };

  std::optional<ColumnDrop> columnDropAt(float x, float y) const;

  void cancelColumnDrag();

  void updateColumnDrag();

  void finishColumnDrag(float x, float y);

  void layoutColumnDrag(Renderer& renderer);

  Button* columnHeader(files::DetailsColumn column) const;

  std::optional<files::DetailsColumnOrder> movedColumnOrder(files::DetailsColumn column, int direction) const;

  void setDetailsColumnOrder(files::DetailsColumnOrder order);

  void changeSort(files::SortOptions options);

  void sortBy(files::SortField field);

  void setViewMode(files::ViewMode mode);

  void applyViewMode();

  float gridHeight() const;

  float gridIconSize() const;

  void changeGridZoom(int requested);

  bool zoomFromKey(const KeyboardEvent& event);

  bool zoomFromScroll(const PointerEvent& event);

  bool listKey(const KeyboardEvent& event);

  void syncBrowser();

  std::uint32_t resizeEdges(float x, float y) const;

  void prepareFrame(bool, bool layout);

  void buildScene();

  void revealMenuHighlight();
  void dismissMenu();
  void closeMenu();
  static LayoutRect sceneRect(const Node& node);

  void showPopup(
      std::unique_ptr<Flex> root, ContextMenuControl* menu, LayoutRect anchor, float width, float height,
      bool alignLeft = false
  );

  void showContextMenu(
      std::vector<ContextMenuControlEntry> entries, std::function<void(int)> action, LayoutRect anchor,
      bool alignLeft = false, float preferredWidth = 264.0F
  );

  void openColumnsMenu(const LayoutRect& anchor, std::optional<files::DetailsColumn> column = std::nullopt);

  void toggleHidden();

  void showActionMenu(
      std::initializer_list<std::optional<Action>> actions, bool radio = false, bool back = false,
      std::optional<LayoutRect> anchor = std::nullopt, bool alignLeft = true
  );
  void openSortMenu(std::optional<LayoutRect> anchor = std::nullopt, bool alignLeft = true);
  void openSelectionMenu();
  void openMenu();

  WaylandConnection& m_connection;
  RenderContext& m_renderer;
  TextInputService& m_textInput;
  AnimationManager& m_animations;
  files::WindowSurface* m_surface;
  files::BrowserController m_browser;
  files::MillerColumns m_miller;
  MillerView* m_millerView = nullptr;
  VirtualGridView* m_regularList = nullptr;
  bool m_millerFocusPending = false;
  bool m_restartMiller = false;
  const files::BrowserController* m_lastBrowser = nullptr;
  files::ActivationMode m_activationMode = files::ActivationMode::DoubleClick;
  files::TypingMode m_typingMode = files::TypingMode::Search;
  files::TypeaheadSelection m_typeahead;
  std::string m_typeaheadPreedit;
  Timer m_typeaheadTimer;
  std::uint64_t m_typeaheadContext = 0;
  NameEditor* m_patternEditor = nullptr;
  std::string m_selectionPattern = "*";
  files::FileIconCache& m_icons;
  files::ThumbnailService m_thumbnails;
  files::PreviewService m_preview;
  FilePreviewView* m_previewView = nullptr;
  FilePreviewView* m_quickPreview = nullptr;
  Flex* m_appChoiceContainer = nullptr;
  ScrollView* m_appChoiceScroll = nullptr;
  std::optional<LayoutRect> m_previewBounds;
  bool m_thumbnailClearing = false;
  std::string m_thumbnailError;
  std::uint64_t m_thumbnailRevision = 0;
  std::uint64_t m_thumbnailContext = 0;
  files::FileViewAdapter m_listAdapter;
  files::FileViewAdapter m_gridAdapter;
  files::ViewPreferenceWriter& m_preferences;
  files::BrowserSettings m_savedSettings;
  std::optional<files::Settings> m_pendingSettings;
  bool m_settingsDirty = false;
  BrowserSettingsView* m_settingsView = nullptr;
  std::shared_ptr<int> m_settingsLifetime;
  std::function<void()> m_selectDismiss;
  ScrollView* m_aboutScroll = nullptr;
  Label* m_aboutMessage = nullptr;
  std::optional<float> m_dialogHeight;
  float m_dialogWidth = 588.0F;
  std::optional<std::pair<int, int>> m_dialogPosition;
  bool m_dialogResizePending = false;
  files::PinService& m_pins;
  files::FileLauncher m_launcher;
  files::PlacesSidebar* m_places = nullptr;
  bool m_placesDirty = true;
  std::string m_actionError;
  std::vector<files::Location> m_revealTargets;
  bool m_revealProperties = false;
  std::optional<Drag> m_drag;
  std::shared_ptr<const files::FileIconImage> m_dragArtwork;
  std::uint32_t m_fileDragSerial = 0;
  bool m_pendingFilePaste = false;
  bool m_externalHover = false;
  bool m_externalDraggingHere = false;
  bool m_refreshExternalDrop = false;
  bool m_systemDrag = false;
  Timer m_externalScroll;
  float m_externalScrollDelta = 0;
  bool m_externalScrollInPlaces = false;
  std::optional<std::size_t> m_externalPinSlot;
  std::optional<DropTarget> m_externalDropTarget;
  std::optional<files::ClipboardFiles> m_clipboardTransfer;
  files::DropCompletion m_dropCompletion;
  Timer m_dragScroll;
  float m_dragScrollDelta = 0;
  float m_pointerX = 0;
  float m_pointerY = 0;
  std::uint32_t m_popupSerial = 0;
  files::FileIcon* m_dragIcon = nullptr;
  Flex* m_dragCount = nullptr;
  Label* m_dragCountLabel = nullptr;
  Box* m_folderDropHighlight = nullptr;
  Flex* m_filePane = nullptr;
  std::vector<std::pair<files::Location, Button*>> m_dropBreadcrumbs;
  std::optional<DropTarget> m_lastDropTarget;
  bool m_dragScrollInPlaces = false;
  files::ArchiveService m_archives;
  ArchiveView* m_archiveView = nullptr;
  std::optional<files::Location> m_archiveSource;
  bool m_archiveWanted = false, m_archiveDirty = false, m_transferConflictArchive = false;
  files::TransferService m_transfers;
  files::TrashService m_trash;
  files::RestoreService m_restore;
  bool m_transferConflictRestore = false;
  files::EmptyTrashService m_emptyTrash;
  bool m_emptyTrashDirty = false;
  bool m_emptyTrashDialog = false;
  bool m_selectedTrashDelete = false;
  Label* m_emptyTrashMessage = nullptr;
  Label* m_emptyTrashSize = nullptr;
  Button* m_emptyTrashConfirm = nullptr;
  bool m_trashDirty = false;
  files::RenameService m_renamer;
  files::BatchRenameService m_batchRenamer;
  files::PermissionService m_permissions;
  PermissionEditor* m_permissionEditor = nullptr;
  AclEditor* m_aclEditor = nullptr;
  BulkAclEditor* m_bulkAclEditor = nullptr;
  Flex *m_permissionBasic = nullptr, *m_permissionAdvanced = nullptr;
  Button *m_permissionAdvancedOpen = nullptr, *m_permissionAdvancedBack = nullptr;
  bool m_aclActive = false;
  std::size_t m_permissionResultTab = 2;
  PermissionResultsView* m_permissionResults = nullptr;
  Button *m_permissionReport = nullptr, *m_permissionCancel = nullptr;
  Label* m_permissionStatus = nullptr;
  Segmented* m_permissionTabs = nullptr;
  ScrollView* m_permissionScroll = nullptr;
  Button* m_permissionSave = nullptr;
  bool m_permissionDirty = false;
  BatchRenameDialog* m_batchRenameDialog = nullptr;
  bool m_batchRenameDirty = false;
  bool m_batchRenameReported = false;
  std::vector<files::Location> m_batchReveal;
  std::uint64_t m_batchRevealContext = 0;
  files::CreateService m_creator;
  files::TemplateCatalog m_templates;
  Timer m_operationProgress;
  Button* m_cancelOperation = nullptr;
  Button* m_retryTransfer = nullptr;
  Box* m_placeDropLine = nullptr;
  std::string m_preferenceError;
  std::size_t m_gridZoom = 1;
  double m_zoomScroll = 0;
  files::ViewMode m_viewMode;
  files::WaylandTextClipboard& m_clipboard;
  files::TerminalLauncher m_terminal;
  files::PathCompletion m_pathCompletion;
  files::PathCompletion m_siblingCompletion;
  files::PathSuggestions m_completionItems;
  std::string m_completionText;
  std::uint64_t m_completionContext = 0;
  std::optional<std::size_t> m_completionIndex;
  bool m_completionDirty = false;
  ContextMenuControl* m_completionControl = nullptr;
  Flex* m_completionFrame = nullptr;
  int m_sidebarWidth = files::kDefaultSidebarWidth;
  bool m_showSidebar = true;
  int m_sidebarStartWidth = files::kDefaultSidebarWidth;
  int m_sidebarStartVisibleWidth = files::kDefaultSidebarWidth;
  float m_sidebarPressX = 0;
  bool m_resizingSidebar = false;
  InputArea* m_sidebarResize = nullptr;
  Box* m_sidebarResizeLine = nullptr;
  std::optional<files::ViewMode> m_appliedViewMode;
  std::optional<files::EntryId> m_viewAnchor;
  std::uint64_t m_visitSerial = 0;
  std::optional<files::BrowseViewport> m_pendingHistoryViewport;
  std::optional<files::EntryId> m_keyboardReveal;
  Node* m_root = nullptr;
  std::unique_ptr<Node> m_detachedRoot;
  Node* m_dragOverlay = nullptr;
  std::unique_ptr<Node> m_detachedDragOverlay;
  Flex* m_body = nullptr;
  Flex* m_paneHeader = nullptr;
  Label* m_paneTitle = nullptr;
  Flex* m_toolbar = nullptr;
  Node* m_sidebarBundle = nullptr;
  std::unique_ptr<Node> m_detachedSidebar;
  LayoutRect m_pageBounds{};
  std::optional<InputDispatcher::TabFocusSnapshot> m_stashedFocus;
  std::optional<files::BrowseViewport> m_inactiveViewport;
  std::function<void(WorkspaceCommand, std::optional<files::Location>)> m_workspaceRequest;
  std::function<std::optional<std::pair<files::Location, LayoutRect>>(float, float)> m_foreignDrop;
  bool m_visible = false, m_active = false;
  files::TabId m_workspaceId = 0;
  std::uint64_t m_nextFocusKey = 0;
  std::function<bool()> m_popupAvailable, m_hasOther;
  std::function<void()> m_clearThumbnails;
  bool m_cachePaused = false;
  std::optional<files::ActionRequest> m_pendingWorkspaceRequest;

  InputDispatcher m_dispatcher;
  Flex* m_titlebar = nullptr;
  Flex* m_paneContentFrame = nullptr;
  Input* m_location = nullptr;
  Input* m_filterInput = nullptr;
  Flex* m_searchArea = nullptr;
  Flex* m_locationArea = nullptr;
  ScrollView* m_breadcrumbScroll = nullptr;
  Flex* m_breadcrumbs = nullptr;
  struct BreadcrumbPart {
    files::Location location;
    Button* button;
    Node* separator;
  };
  std::vector<BreadcrumbPart> m_breadcrumbParts;
  Button* m_breadcrumbOverflow = nullptr;
  std::size_t m_firstBreadcrumb = 0;
  float m_breadcrumbLayoutWidth = -1;
  float m_breadcrumbRenderScale = 0;
  std::uint64_t m_breadcrumbMetricsGeneration = 0;
  struct LocationBlankClick {
    files::Location location;
    std::uint64_t context;
    std::chrono::steady_clock::time_point time;
    float x, y;
  };
  std::optional<LocationBlankClick> m_blankLocationClick;
  std::optional<files::Location> m_lastBreadcrumb;
  std::optional<files::Location> m_breadcrumbEditTarget;
  std::chrono::steady_clock::time_point m_breadcrumbClickTime;
  float m_breadcrumbClickX = 0;
  float m_breadcrumbClickY = 0;
  bool m_breadcrumbsDirty = true;
  bool m_editingLocation = false;
  bool m_filterOpen = false;
  Label* m_status = nullptr;
  Label* m_freeSpace = nullptr;
  bool m_freeSpaceAvailable = false;
  Label* m_notice = nullptr;
  Flex* m_noticeArea = nullptr;
  VirtualGridView* m_list = nullptr;
  Flex* m_listHeader = nullptr;
  Segmented* m_viewSwitch = nullptr;
  Button* m_nameHeader = nullptr;
  Button* m_sizeHeader = nullptr;
  Button* m_modifiedHeader = nullptr;
  Button* m_typeHeader = nullptr;
  files::DetailsColumns m_detailsColumns;
  files::DetailsColumnWidths m_columnWidths = files::kDefaultDetailsColumnWidths;
  std::optional<ColumnResize> m_columnResize;
  std::array<InputArea*, 4> m_columnResizeHandles{};
  std::array<Separator*, 4> m_columnResizeLines{};
  Flex* m_detailsHeaderViewport = nullptr;
  Box* m_detailsScrollHost = nullptr;
  Scrollbar* m_detailsScrollbar = nullptr;
  float m_detailsOffset = 0.0F;
  Button* m_revealDetailsHeader = nullptr;
  InputArea* m_lastDetailsFocus = nullptr;
  files::DetailsColumnVisibility m_columnVisibility;
  files::DetailsColumnOrder m_columnOrder = files::kDefaultDetailsColumnOrder;
  files::DetailsColumnOrder m_appliedColumnOrder = files::kDefaultDetailsColumnOrder;
  std::optional<ColumnDrag> m_columnDrag;
  bool m_columnHoverDirty = false;
  Box* m_columnDropLine = nullptr;
  Flex* m_detailsHeaderRow = nullptr;
  Button* m_menuButton = nullptr;
  Button* m_back = nullptr;
  Button* m_forward = nullptr;
  Button* m_up = nullptr;
  Button* m_sortButton = nullptr;
  std::string m_displayedLocation;
  bool m_browserDirty = true;
  bool m_fileKeyboardFocus = false;
  bool m_locationInvalid = false;
  std::unique_ptr<PopupSurface> m_menuSurface;
  std::unique_ptr<Flex> m_menuRoot;
  ContextMenuControl* m_menu = nullptr;
  std::unique_ptr<PopupSurface> m_documentSurface;
  std::unique_ptr<Flex> m_documentRoot;
  ContextMenuControl* m_documentMenu = nullptr;
  ScrollView* m_documentScroll = nullptr;
  InputDispatcher m_documentDispatcher;
  std::optional<std::size_t> m_documentRow;
  std::uint64_t m_documentGeneration = 0;
  bool m_closeDocumentPending = false;
  bool m_documentKeyboard = false;
  ScrollView* m_menuScroll = nullptr;
  NameEditor* m_placeEditor = nullptr;
  NameEditor* m_fileRenameEditor = nullptr;
  NameEditor* m_createEditor = nullptr;
  LinkDialog* m_linkDialog = nullptr;
  bool m_linkPreparing = false;
  std::optional<files::Location> m_createParent;
  bool m_creationPreparing = false;
  bool m_creationDirty = false;
  std::optional<files::DirectoryEntry> m_fileRenameTarget;
  bool m_fileRenamePreparing = false;
  bool m_renameDirty = false;
  std::optional<files::Bookmark> m_placeRenameTarget;
  std::optional<std::string> m_pendingPlaceLabel;
  std::optional<files::Location> m_restorePlaceFocus;
  InputDispatcher m_menuDispatcher;
  std::uint64_t m_menuGeneration = 0;
  std::optional<std::uint64_t> m_activeTransferConflict;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
