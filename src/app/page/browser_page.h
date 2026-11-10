#pragma once

#include "app/operation_hub.h"
#include "app/page/about_controller.h"
#include "app/page/active_dialog.h"
#include "app/page/application_chooser_controller.h"
#include "app/page/archive_controller.h"
#include "app/page/batch_rename_controller.h"
#include "app/page/create_controller.h"
#include "app/page/details_header_controller.h"
#include "app/page/drag_drop_controller.h"
#include "app/page/file_keyboard_controller.h"
#include "app/page/location_bar_controller.h"
#include "app/page/menu_controller.h"
#include "app/page/page_context.h"
#include "app/page/preview_controller.h"
#include "app/page/properties_controller.h"
#include "app/page/rename_controller.h"
#include "app/page/settings_controller.h"
#include "app/page/sidebar_controller.h"
#include "app/page/status_bar_controller.h"
#include "app/page/transfer_controller.h"
#include "app/page/trash_controller.h"
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
#include "operations/template_catalog.h"
#include "platform/desktop_integration.h"
#include "platform/file_launcher.h"
#include "platform/terminal_launcher.h"
#include "render/animation/animation_manager.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "thumbnails/preview_service.h"
#include "ui/browser/file_icon.h"
#include "ui/browser/file_tooltip.h"
#include "ui/browser/file_view.h"
#include "ui/browser/places_sidebar.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/input.h"
#include "ui/controls/miller_view.h"
#include "ui/controls/name_editor.h"
#include "ui/controls/scroll_view.h"
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
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

class BrowserPage final : public PageContext, public OperationHub::Owner {
public:
  using Action = files::BrowserAction;

  // `id` is the page's TabId in the workspace; the page shows the jobs `hub` routes to it. `dispatcher` is the input
  // dispatcher of the window that owns `surface`.
  BrowserPage(
      WaylandConnection& connection, RenderContext& renderer, TextInputService& textInput,
      const files::BrowserSettings& settings, files::WaylandTextClipboard& clipboard, files::WindowSurface& surface,
      InputDispatcher& dispatcher, AnimationManager& animations, files::PinService& pins, files::DriveMonitor& drives,
      files::FileIconCache& icons, files::ThumbnailService& thumbnails, files::ViewPreferenceWriter& preferences,
      OperationHub& hub, files::TabId id
  );

  files::BrowserController& browser() override;
  const files::BrowserController& browser() const;
  void focusMiller(std::size_t index, bool reveal = true);

  ~BrowserPage();

  void iconThemeChanged();

  bool initialize(const std::optional<std::string>& initialLocation);

  // Select `items` once the current folder finishes loading; optionally open their Properties.
  void revealItems(std::vector<files::Location> items, bool properties);

  // True while page-local work that outlives the window runs: launching, saving places or preferences, a paste.
  bool busy() const;
  void shutdown();
  // OperationHub::Owner: hands the job to the controller of its kind.
  void jobChanged(OperationJob& job) override;
  // The shared thumbnail service changed.
  void thumbnailsChanged();

  files::BrowserSettings options() const;
  std::optional<files::Location> location() const;
  std::string title() const;
  std::optional<files::FileTooltipTarget> fileTooltipAt(float x, float y) const;
  void preferencesChanged(const std::string& error);
  void placesChanged();
  void clipboardChanged();
  void clearExternalDrop();
  const std::vector<files::Bookmark>* draggedFolders() const;
  files::FileDropTarget externalDrop(float x, float y, const std::vector<files::Bookmark>* pins);
  files::FileDropTarget externalDestination(const files::Location& destination, LayoutRect rect);
  void iconsReady();
  void reusePlacesFrom(BrowserPage& previous);
  void workspaceError(const std::string& message);
  bool owns(const Node* node) const;
  // The window dispatcher's focus moved to `focused`.
  void focusChanged(InputArea* focused);
  bool completeLocationOnTab(std::uint32_t modifiers);
  void stampFocusKeys(Node& node);
  void duplicateFrom(BrowserPage& source);
  void open(const files::Location& location);
  void setWorkspaceRequest(std::function<void(WorkspaceCommand, std::optional<files::Location>)> callback);
  void configurationChanged();
  void setHasOther(std::function<bool()> callback);
  void setPopupAvailable(std::function<bool()> callback);
  void setForeignDrop(std::function<std::optional<std::pair<files::Location, LayoutRect>>(float, float)> callback);
  bool popupOwns(wl_surface* surface) const;
  bool popup() const;
  bool overlay() const override;
  bool previewVisible() const;
  int previewWidth() const;
  float previewAreaWidth() const;
  void savePreviewWidth(int width);
  void setPreviewBounds(std::optional<LayoutRect> bounds);
  void layoutNavigation(Renderer& renderer);
  bool dragging() const;
  bool contains(float x, float y) const;
  files::WindowSurface* boundSurface() const;
  // Moves the page to another window: its surface and that window's input dispatcher.
  void rebind(files::WindowSurface& surface, InputDispatcher& dispatcher);
  // `roundCorner`: this pane starts the content plane beside the sidebar and rounds its top-left corner.
  void prepare(LayoutRect bounds, bool active, bool roundCorner);
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
  files::ActionContext actionContext() const;
  files::ActionRequest actionRequest(bool withTargets) const override;
  files::ActionRequest requestFor(const std::vector<files::DirectoryEntry>& entries) const override;
  files::ActionState actionState(Action action, const files::ActionRequest& request) const override;
  void dispatchAction(Action action) override;
  void dispatchAction(Action action, const files::ActionRequest& request) override;
  void runAction(Action action, const files::ActionRequest& request, const std::vector<files::DirectoryEntry>& targets);
  void copyFiles(Action action, const std::vector<files::DirectoryEntry>& targets);
  void copyPaths(Action action, const files::ActionRequest& request, const std::vector<files::DirectoryEntry>& targets);
  void openTerminal(const files::Location& target);
  void sortAction(Action action);
  void openTargets(const std::vector<files::DirectoryEntry>& targets);
  void pinTargets(const std::vector<files::DirectoryEntry>& targets);
  void unpinTargets(const std::vector<files::DirectoryEntry>& targets);
  void openPatternSelection(const files::ActionRequest& request);

  void actionError(std::string message) override;
  void setActionMessage(std::string message) override;
  void revealOnReload(std::vector<files::Location> targets) override;
  void restorePlaceFocus(files::Location location) override;

  bool targetsCurrent(const std::vector<files::DirectoryEntry>& targets) override;
  bool requestCurrent(const files::ActionRequest& request, files::ActionScope scope) const override;
  bool operationsBusy() const override;
  PageJobs& jobs() override;
  bool visible() const override;
  bool active() const override;
  bool thumbnailsPaused() const override;
  bool popupAvailable() override;
  void prepareOverlay() override;
  Node& sceneRoot() override;
  const files::BrowserSettings& settings() const override;
  bool hasOtherPane() const override;
  void requestWorkspace(
      WorkspaceCommand command, std::optional<files::Location> location, std::optional<files::ActionRequest> pending
  ) override;
  LayoutRect menuAnchor() const override;
  Node& upButton() override;
  void requestSync() override;
  float pointerX() const override;
  float pointerY() const override;
  VirtualGridView* fileList() override;
  Flex& filePane() override;
  MillerView& millerView() override;
  files::ViewMode viewMode() const override;
  float gridIconSize() const override;
  void captureViewAnchor() override;
  void resetFileView() override;

  void operationProgressTick();

  void showContaining(const files::Location& location);

  void applySettings(const files::BrowserSettings& settings, bool force) override;

  void showCenteredDialog(std::unique_ptr<Flex> root, float preferredWidth) override;

  void cancelDrag() override;

  void setViewMode(files::ViewMode mode);
  void applyViewMode();
  float gridHeight() const;
  void changeSort(files::SortOptions options);
  void changeGridZoom(int requested);
  bool zoomFromKey(const KeyboardEvent& event);
  bool zoomFromScroll(const PointerEvent& event);
  void toggleHidden();

  // Input: pointer() and keyboard() run these hooks and the controllers' hooks in a fixed order.
  bool openInNewTab(float x, float y);
  void pointerOverlay(InputDispatcher& input, const PointerEvent& event, float x, float y);
  bool millerPointer(const PointerEvent& event, float x, float y);
  void dispatchPointer(InputDispatcher& input, const PointerEvent& event, float x, float y);
  std::uint32_t resizeEdges(float x, float y) const;
  bool escapeGesture(const KeyboardEvent& event);
  // Dispatches the first of `actions` whose table shortcut matches the key.
  bool dispatchShortcut(const KeyboardEvent& event, std::initializer_list<Action> actions);
  bool shortcutKey(const KeyboardEvent& event);
  bool windowKey(const KeyboardEvent& event);
  bool navigationKey(const KeyboardEvent& event, bool editing);
  bool millerKey(const KeyboardEvent& event);

  // Frame: prepareFrame() syncs the page and its controllers, then layoutFrame() lays them out.
  void syncBrowser();
  void revealPendingTargets();
  void prepareFrame(bool, bool layout);
  void layoutFrame(Renderer& renderer);

  // Scene.
  void buildScene();
  std::unique_ptr<Flex> buildTitlebar();
  std::unique_ptr<Flex> buildToolbar();
  std::unique_ptr<Flex> buildFilePane();

  void dismissMenu() override;
  void closeMenu() override;

  void showPopup(
      std::unique_ptr<Flex> root, ContextMenuControl* menu, LayoutRect anchor, float width, float height,
      bool alignLeft = false
  ) override;
  void setDialogSize(float width, float height) override;
  PopupSurface* menuSurface() override;
  InputDispatcher& menuDispatcher() override;
  ActiveDialog& activeDialog() override;
  ContextMenuControl* contextMenu() override;
  std::uint32_t popupSerial() override;
  files::WaylandTextClipboard& clipboard() override;
  files::WindowSurface& surface() override;
  InputDispatcher& dispatcher() override;
  void focusFileList() override;

  WaylandConnection& m_connection;
  RenderContext& m_renderer;
  TextInputService& m_textInput;
  AnimationManager& m_animations;
  files::WindowSurface* m_surface;
  InputDispatcher* m_dispatcher;
  files::BrowserController m_browser;
  files::MillerColumns m_miller;
  MillerView* m_millerView = nullptr;
  VirtualGridView* m_regularList = nullptr;
  bool m_millerFocusPending = false;
  bool m_restartMiller = false;
  const files::BrowserController* m_lastBrowser = nullptr;
  files::ActivationMode m_activationMode = files::ActivationMode::DoubleClick;
  NameEditor* m_patternEditor = nullptr;
  std::string m_selectionPattern = "*";
  files::FileIconCache& m_icons;
  files::ThumbnailService& m_thumbnails;
  files::PreviewService m_preview;
  bool m_thumbnailClearing = false;
  std::string m_thumbnailError;
  std::uint64_t m_thumbnailRevision = 0;
  std::uint64_t m_thumbnailContext = 0;
  files::FileViewAdapter m_listAdapter;
  files::FileViewAdapter m_gridAdapter;
  files::ViewPreferenceWriter& m_preferences;
  files::BrowserSettings m_savedSettings;
  std::optional<float> m_dialogHeight;
  float m_dialogWidth = 588.0F;
  std::optional<std::pair<int, int>> m_dialogPosition;
  bool m_dialogResizePending = false;
  files::PinService& m_pins;
  files::DriveMonitor& m_drives;
  files::FileLauncher m_launcher;
  std::vector<files::Location> m_revealTargets;
  bool m_revealProperties = false;
  float m_pointerX = 0;
  float m_pointerY = 0;
  std::uint32_t m_popupSerial = 0;
  Flex* m_filePane = nullptr;
  OperationHub& m_hub;
  PageJobs m_jobs;
  TrashController m_trashController{*this};
  RenameController m_renameController{*this, m_pins};
  PropertiesController m_propertiesController{*this, m_icons};
  BatchRenameController m_batchRename{*this};
  files::TemplateCatalog m_templates;
  CreateController m_createController{*this, m_templates, m_connection, m_renderer, m_animations};
  ArchiveController m_archiveController{*this, m_launcher, m_icons, m_connection};
  TransferController m_transferController{*this, [this] { operationProgressTick(); }};
  MenuController m_menuController{
      *this,
      m_pins,
      m_transferController,
      m_terminal,
      m_siblingCompletion,
      m_createController,
      m_renameController,
      m_propertiesController,
      m_detailsHeader,
      m_locationBar
  };
  SettingsController m_settingsController{*this,        m_preferences, m_thumbnails,
                                          m_connection, m_renderer,    [this] {
        m_thumbnails.clearCache(); }};
  AboutController m_aboutController{*this, m_launcher};
  ApplicationChooserController m_applicationChooser{*this, m_launcher, m_icons};
  PreviewController m_previewController{*this, m_preview, m_thumbnails, m_icons, m_renderer, m_applicationChooser};
  DetailsHeaderController m_detailsHeader{*this, m_preferences, m_listAdapter, m_gridAdapter, m_menuController};
  LocationBarController m_locationBar{*this, m_connection, m_fileKeys, m_menuController};
  SidebarController m_sidebar{*this,        m_pins,           m_drives,           m_preferences,
                              m_animations, m_menuController, m_renameController, m_dragDrop};
  DragDropController m_dragDrop{*this,   m_connection, m_pins,    m_transferController, m_thumbnails,
                                m_icons, m_miller,     m_sidebar, m_locationBar};
  FileKeyboardController m_fileKeys{*this, m_textInput, m_miller, m_menuController, m_locationBar, m_statusBar};
  StatusBarController m_statusBar{*this, m_pins, m_transferController, m_settingsController, m_fileKeys};
  std::size_t m_gridZoom = 1;
  double m_zoomScroll = 0;
  files::ViewMode m_viewMode;
  files::WaylandTextClipboard& m_clipboard;
  files::TerminalLauncher m_terminal;
  files::PathCompletion m_siblingCompletion;
  std::optional<files::ViewMode> m_appliedViewMode;
  std::optional<files::EntryId> m_viewAnchor;
  std::uint64_t m_visitSerial = 0;
  std::optional<files::BrowseViewport> m_pendingHistoryViewport;
  Node* m_root = nullptr;
  std::unique_ptr<Node> m_detachedRoot;
  Flex* m_body = nullptr;
  Flex* m_paneHeader = nullptr;
  Label* m_paneTitle = nullptr;
  Flex* m_toolbar = nullptr;
  LayoutRect m_pageBounds{};
  std::optional<InputDispatcher::TabFocusSnapshot> m_stashedFocus;
  std::optional<files::BrowseViewport> m_inactiveViewport;
  std::function<void(WorkspaceCommand, std::optional<files::Location>)> m_workspaceRequest;
  bool m_visible = false, m_active = false;
  files::TabId m_workspaceId;
  std::uint64_t m_nextFocusKey = 0;
  std::function<bool()> m_popupAvailable, m_hasOther;
  std::optional<files::ActionRequest> m_pendingWorkspaceRequest;

  Flex* m_titlebar = nullptr;
  Flex* m_paneContentFrame = nullptr;
  VirtualGridView* m_list = nullptr;
  Segmented* m_viewSwitch = nullptr;
  Button* m_menuButton = nullptr;
  Button* m_back = nullptr;
  Button* m_forward = nullptr;
  Button* m_up = nullptr;
  Button* m_sortButton = nullptr;
  bool m_browserDirty = true;
  std::unique_ptr<PopupSurface> m_menuSurface;
  ActiveDialog m_activeDialog;
  InputDispatcher m_menuDispatcher;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
