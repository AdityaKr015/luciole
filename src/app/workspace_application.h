#pragma once

#include "app/file_manager_service.h"
#include "app/operation_hub.h"
#include "app/page/browser_page.h"
#include "app/palette_watcher.h"
#include "browser/tab_workspace.h"
#include "thumbnails/thumbnail_service.h"

#include <csignal>
#include <map>
#include <memory>

// Set by SIGINT, SIGTERM and Ctrl+Q; every window closes and the main loop stops once it is non-zero.
extern volatile std::sig_atomic_t g_stopRequested;

class WorkspaceApplication {
  struct Host {
    files::WindowId id;
    files::WindowSurface surface;
    std::unique_ptr<Node> root;
    std::unique_ptr<Node> toolbar;
    Node* toolbarNode = nullptr;
    Flex* strip = nullptr;
    ScrollView* tabScroll = nullptr;
    Button* newTabButton = nullptr;
    Button* splitButton = nullptr;
    InputArea* divider = nullptr;
    InputArea* previewDivider = nullptr;
    Box* previewResizeLine = nullptr;
    InputDispatcher dispatcher;
    TooltipPresenter* tooltip = nullptr;
    std::vector<std::pair<files::TabId, Button*>> buttons;
    std::map<files::TabId, std::pair<Button*, ColorRole>> closeButtons;
    files::TabId revealedTab = 0;
    std::uint32_t revealedTabWidth = 0;
    std::vector<std::pair<files::TabId, std::string>> labels;
    std::vector<files::TabId> mounted;
    files::TabId chromeOwner = 0;
    std::optional<files::TabId> pressedTab;
    float pressX = 0, pressY = 0;
    bool tabDrag = false, resizing = false, closed = false;
    bool decorated = false;
    float pointerX = 0, pointerY = 0;
    float resizeStart = 0.5F;
    std::optional<int> previewDragWidth;
    LayoutRect browsingBounds{};
    Host(files::WindowId value, WaylandConnection& connection) : id(value), surface(connection) {}
  };

public:
  WorkspaceApplication(
      WaylandConnection& connection, RenderContext& renderer, TextInputService& input,
      files::WaylandTextClipboard& clipboard, files::BrowserSettings settings
  );
  SharedTextureCache& imageTextures();

  ~WorkspaceApplication();
  void start(const std::optional<std::string>& initial);
  // FileManager1 request: one new window, one tab per folder; items are selected in their parent.
  void show(const files::FileManagerRequest& request);
  bool hasWindows() const;
  void focus(wl_surface* surface, bool entered);
  void outputChanged(wl_output* output);
  void iconsChanged();
  bool running();
  void keyboard(const KeyboardEvent& event);
  void pointer(const PointerEvent& event);

private:
  void clearExternalDrops();
  static void dispatch(InputDispatcher& dispatcher, const PointerEvent& event);
  files::WindowId newWindow();
  files::TabId newTab(files::WindowId window, std::optional<files::Location> location, files::TabId duplicate = 0);
  files::TabId newPane(files::WindowId window, std::optional<files::Location> location, files::TabId duplicate = 0);
  files::TabId createPage(
      files::WindowId window, files::TabId id, std::optional<files::Location> location, files::TabId duplicate,
      files::TabId previous
  );
  void select(files::WindowId window, files::TabId tab);
  void defer(
      files::WindowId window, files::TabId tab, WorkspaceCommand command,
      std::optional<files::Location> location = std::nullopt
  );
  void execute(
      files::WindowId window, files::TabId tab, WorkspaceCommand command,
      const std::optional<files::Location>& destination
  );
  void releaseChrome(Host& host);
  void tabMenu(Host& host, files::TabId tab, float x, float y);
  static Button::ButtonPalette closeTabPalette(ColorRole foreground);
  void rebuildStrip(Host& host, const files::TabWindow& window);
  void syncTooltip(Host& host);
  void prepare(Host& host);
  WaylandConnection& m_connection;
  RenderContext& m_renderer;
  TextInputService& m_input;
  files::WaylandTextClipboard& m_clipboard;
  files::BrowserSettings m_defaults;
  // Applies colors.toml before any surface is built, then follows its changes.
  files::PaletteWatcher m_palette;
  AnimationManager m_animations;
  files::ViewPreferenceWriter m_preferences;
  files::PinService m_pins;
  files::DriveMonitor m_drives;
  files::FileIconCache m_icons;
  // One thumbnail pipeline for every page.
  files::ThumbnailService m_thumbnails;
  files::TabWorkspace m_model;
  // File jobs outlive the page that started them; pages must go before the hub.
  OperationHub m_hub{m_model};
  std::map<files::TabId, std::unique_ptr<BrowserPage>> m_pages;
  std::map<files::WindowId, std::unique_ptr<Host>> m_hosts;
  files::WindowId m_focused = 0;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
