#include "app/application.h"

#include "app/deferred_call_poll_source.h"
#include "app/glib_poll_source.h"
#include "app/main_loop.h"
#include "app/timer_poll_source.h"
#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/animation/animation_manager.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "ui/builders.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/input.h"
#include "ui/palette.h"
#include "wayland/popup_surface.h"
#include "wayland/text_input_service.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <array>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <memory>
#include <print>
#include <string>
#include <utility>

namespace {
  volatile std::sig_atomic_t g_stopRequested = 0;
  void stopRequested(int) { g_stopRequested = 1; }

  class KeyRepeatPollSource final : public PollSource {
  public:
    explicit KeyRepeatPollSource(WaylandSeat& seat) : m_seat(seat) {}
    [[nodiscard]] int pollTimeoutMs() const override { return m_seat.repeatPollTimeoutMs(); }
    void dispatch(const std::vector<pollfd>&, std::size_t) override { m_seat.repeatTick(); }

  private:
    void doAddPollFds(std::vector<pollfd>&) override {}
    WaylandSeat& m_seat;
  };

  void initializeKeys() {
    const std::array<std::pair<KeybindAction, std::uint32_t>, 8> keys{{
        {KeybindAction::Cancel, XKB_KEY_Escape},
        {KeybindAction::Left, XKB_KEY_Left},
        {KeybindAction::Right, XKB_KEY_Right},
        {KeybindAction::Up, XKB_KEY_Up},
        {KeybindAction::Down, XKB_KEY_Down},
        {KeybindAction::TabNext, XKB_KEY_Tab},
        {KeybindAction::Delete, XKB_KEY_Delete},
        {KeybindAction::Save, XKB_KEY_s},
    }};
    for (const auto& [action, sym] : keys) {
      KeybindMatcher::setMatcher(action, [action, sym](std::uint32_t key, std::uint32_t modifiers) {
        return key == sym && modifiers == (action == KeybindAction::Save ? KeyMod::Ctrl : 0U);
      });
    }
    KeybindMatcher::setMatcher(KeybindAction::Validate, [](std::uint32_t sym, std::uint32_t modifiers) {
      return modifiers == 0 && KeySymbol::isEnterOrSpace(sym);
    });
    KeybindMatcher::setMatcher(KeybindAction::Copy, [](std::uint32_t sym, std::uint32_t modifiers) {
      return sym == XKB_KEY_c && modifiers == KeyMod::Ctrl;
    });
    KeybindMatcher::setMatcher(KeybindAction::TabPrevious, [](std::uint32_t sym, std::uint32_t modifiers) {
      return KeySymbol::isTab(sym) && modifiers == KeyMod::Shift;
    });
  }

  class Window {
  public:
    Window(WaylandConnection& connection, RenderContext& renderer, TextInputService& textInput)
        : m_connection(connection), m_renderer(renderer), m_textInput(textInput), m_surface(connection) {
      m_surface.setRenderContext(&renderer);
      m_surface.setAnimationManager(&m_animations);
      m_surface.setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface.requestLayout(); });
      m_surface.setPrepareFrameCallback([this](bool update, bool layout) { prepareFrame(update, layout); });
      m_surface.setUpdateCallback([] {});
      m_surface.setDebugName("files-window");
      m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
        m_connection.input().setCursorShape(serial, shape);
      });
    }

    ~Window() {
      closeMenu();
      m_dispatcher.setSceneRoot(nullptr);
      m_surface.setSceneRoot(nullptr);
      m_root.reset();
    }

    bool initialize() {
      ToplevelSurfaceConfig config{
          .width = 1080,
          .height = 720,
          .minWidth = 620,
          .minHeight = 400,
          .title = "Noctalia Files",
          .appId = "dev.noctalia.Files",
      };
      return m_surface.initialize(nullptr, config);
    }

    [[nodiscard]] bool running() const { return m_surface.isRunning(); }

    void outputChanged(wl_output* output) {
      if (m_connection.outputForSurface(m_surface.wlSurface()) == output) {
        m_surface.onSurfaceOutputEnter(m_surface.wlSurface(), output);
      }
      if (m_menuSurface != nullptr && m_connection.outputForSurface(m_menuSurface->wlSurface()) == output) {
        m_menuSurface->onSurfaceOutputEnter(m_menuSurface->wlSurface(), output);
      }
    }

    void pointer(const PointerEvent& event) {
      auto* dispatcher = event.surface == m_surface.wlSurface()                     ? &m_dispatcher
          : m_menuSurface != nullptr && event.surface == m_menuSurface->wlSurface() ? &m_menuDispatcher
                                                                                    : nullptr;
      if (dispatcher == nullptr) {
        return;
      }
      const auto x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
      if (dispatcher == &m_dispatcher
          && event.type == PointerEvent::Type::Button
          && event.pressed
          && event.button == BTN_LEFT
          && !m_surface.serverDecorated()
          && !m_surface.maximized()
          && !m_surface.fullscreen()) {
        const auto edges = resizeEdges(x, y);
        if (edges != XDG_TOPLEVEL_RESIZE_EDGE_NONE) {
          m_surface.beginResize(event.serial, edges);
          return;
        }
      }
      switch (event.type) {
      case PointerEvent::Type::Enter:
        dispatcher->pointerEnter(x, y, event.serial);
        break;
      case PointerEvent::Type::Leave:
        dispatcher->pointerLeave();
        break;
      case PointerEvent::Type::Motion:
        dispatcher->pointerMotion(x, y, event.serial);
        break;
      case PointerEvent::Type::Button:
        dispatcher->pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
        break;
      case PointerEvent::Type::Axis:
        dispatcher->pointerAxis(
            x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120,
            event.axisLines, event.axisGestureSerial
        );
        break;
      }
    }

    void keyboard(const KeyboardEvent& event) {
      if (m_menuSurface != nullptr) {
        if (!event.pressed) {
          return;
        }
        if (KeySymbol::isEscape(event.sym)) {
          dismissMenu();
        } else if (KeySymbol::isUp(event.sym)) {
          if (m_menu->moveHighlight(-1)) {
            m_menuSurface->requestRedraw();
          }
        } else if (KeySymbol::isDown(event.sym)) {
          if (m_menu->moveHighlight(1)) {
            m_menuSurface->requestRedraw();
          }
        } else if (KeySymbol::isEnter(event.sym)) {
          (void)m_menu->activateHighlighted();
        }
        return;
      }
      if (event.pressed && !event.preedit) {
        if (KeySymbol::isTab(event.sym)) {
          (void)m_dispatcher.cycleTabFocus((event.modifiers & KeyMod::Shift) != 0);
          m_surface.requestRedraw();
          return;
        }
        if (event.sym == XKB_KEY_F11 && !event.repeat) {
          m_surface.toggleFullscreen();
          return;
        }
        if (event.sym == XKB_KEY_q && event.modifiers == KeyMod::Ctrl) {
          m_surface.close();
          return;
        }
        if (event.sym == XKB_KEY_l && event.modifiers == KeyMod::Ctrl && m_location != nullptr) {
          m_dispatcher.setFocus(m_location->inputArea());
          return;
        }
      }
      m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    }

  private:
    std::uint32_t resizeEdges(float x, float y) const {
      const bool left = x<6, right = x> static_cast<float>(m_surface.width()) - 6;
      const bool top = y<6, bottom = y> static_cast<float>(m_surface.height()) - 6;
      if (top) {
        return left ? XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT
            : right ? XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT
                    : XDG_TOPLEVEL_RESIZE_EDGE_TOP;
      }
      if (bottom) {
        return left ? XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT
            : right ? XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT
                    : XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
      }
      return left ? XDG_TOPLEVEL_RESIZE_EDGE_LEFT
          : right ? XDG_TOPLEVEL_RESIZE_EDGE_RIGHT
                  : XDG_TOPLEVEL_RESIZE_EDGE_NONE;
    }

    void prepareFrame(bool, bool layout) {
      if (!m_renderer.makeCurrent(m_surface.renderTarget())) {
        return;
      }
      auto& renderer = m_surface.renderTarget().renderer();
      UiPhaseScope update(UiPhase::Update);
      if (m_root == nullptr) {
        layout = true;
      }
      if (layout) {
        UiPhaseScope phase(UiPhase::Layout);
        if (m_root == nullptr) {
          buildScene();
        }
        const bool clientHeader = !m_surface.serverDecorated() && !m_surface.fullscreen();
        m_titlebar->setVisible(clientHeader);
        m_titlebar->setParticipatesInLayout(clientHeader);
        m_root->arrange(
            renderer, LayoutRect{0, 0, static_cast<float>(m_surface.width()), static_cast<float>(m_surface.height())}
        );
      }
    }

    void buildScene() {
      uiAssertNotRendering("Window::buildScene");
      m_titlebar = nullptr;
      auto title = ui::inputArea(
          {
              .acceptedButtons = InputArea::buttonMask(BTN_LEFT) | InputArea::buttonMask(BTN_RIGHT),
              .flexGrow = 1.0F,
              .onPress =
                  [this](const InputArea::PointerData& data) {
                    if (data.button == BTN_LEFT) {
                      m_surface.beginMove(data.serial);
                    } else {
                      m_surface.showWindowMenu(
                          data.serial, static_cast<int>(data.sceneX), static_cast<int>(data.sceneY)
                      );
                    }
                  },
          },
          ui::label({.text = "Noctalia Files", .fontSize = Style::fontSizeTitle, .fontWeight = FontWeight::SemiBold})
      );
      auto header = ui::row(
          {.out = &m_titlebar,
           .align = FlexAlign::Center,
           .gap = Style::spaceSm,
           .padding = Style::spaceSm,
           .fill = colorSpecFromRole(ColorRole::SurfaceContainer),
           .height = 48.0F},
          std::move(title),
          ui::button({.glyph = "minus", .variant = ButtonVariant::Ghost, .onClick = [this] { m_surface.minimize(); }}),
          ui::button(
              {.glyph = "square", .variant = ButtonVariant::Ghost, .onClick = [this] { m_surface.toggleMaximized(); }}
          ),
          ui::button({.glyph = "x", .variant = ButtonVariant::Ghost, .onClick = [this] { m_surface.close(); }})
      );
      auto toolbar = ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm, .padding = Style::spaceSm},
          ui::button({.glyph = "arrow-left", .enabled = false, .variant = ButtonVariant::Ghost}),
          ui::button({.glyph = "arrow-right", .enabled = false, .variant = ButtonVariant::Ghost}),
          ui::input(
              {.out = &m_location,
               .placeholder = "Location",
               .flexGrow = 1.0F,
               .onSubmit =
                   [this](const std::string&) {
                     m_status->setText("Folder browsing is not available yet.");
                     m_surface.requestLayout();
                   }}
          ),
          ui::button({.glyph = "dots-vertical", .variant = ButtonVariant::Ghost, .onClick = [this] { openMenu(); }})
      );

      auto places = ui::column(
          {.gap = Style::spaceXs,
           .padding = Style::spaceMd,
           .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
           .minWidth = 206.0F}
      );
      const std::array<std::pair<const char*, const char*>, 5> entries{{
          {"home", "Home"},
          {"clock", "Recent"},
          {"star", "Starred"},
          {"network", "Network"},
          {"trash", "Trash"},
      }};
      for (const auto& [glyph, label] : entries) {
        places->addChild(
            ui::row(
                {.align = FlexAlign::Center, .gap = Style::spaceMd, .padding = Style::spaceSm},
                ui::glyph({.glyph = glyph}), ui::label({.text = label})
            )
        );
      }
      auto content = ui::column(
          {.align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .gap = Style::spaceMd,
           .padding = Style::spaceLg,
           .flexGrow = 1.0F},
          ui::glyph({.glyph = "folder", .glyphSize = 56.0F, .color = colorSpecFromRole(ColorRole::Primary)}),
          ui::label({.text = "No folder open", .fontSize = Style::fontSizeHeader}),
          ui::label(
              {.out = &m_status,
               .text = "Folder browsing is not available yet.",
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
          )
      );
      m_root = ui::column(
          {.align = FlexAlign::Stretch,
           .fill = colorSpecFromRole(ColorRole::Surface),
           .fillWidth = true,
           .fillHeight = true},
          std::move(header), std::move(toolbar),
          ui::row(
              {.align = FlexAlign::Stretch, .clipChildren = true, .flexGrow = 1.0F}, std::move(places),
              std::move(content)
          )
      );
      m_root->setAnimationManager(&m_animations);
      m_surface.setSceneRoot(m_root.get());
      m_dispatcher.setSceneRoot(m_root.get());
      m_dispatcher.setTextInputContext(m_surface.wlSurface(), &m_textInput);
    }

    void dismissMenu() {
      const auto generation = m_menuGeneration;
      const std::weak_ptr<int> lifetime = m_lifetime;
      DeferredCall::callLater([this, generation, lifetime] {
        if (!lifetime.expired() && generation == m_menuGeneration) {
          closeMenu();
        }
      });
    }
    void closeMenu() {
      ++m_menuGeneration;
      m_menuDispatcher.setSceneRoot(nullptr);
      if (m_menuSurface != nullptr) {
        m_menuSurface->setSceneRoot(nullptr);
      }
      m_menuRoot.reset();
      m_menu = nullptr;
      m_menuSurface.reset();
    }
    void openMenu() {
      if (m_menuSurface != nullptr) {
        dismissMenu();
        return;
      }
      m_menuSurface = std::make_unique<PopupSurface>(m_connection);
      m_menuSurface->setRenderContext(&m_renderer);
      m_menuSurface->setAnimationManager(&m_animations);
      m_menuSurface->setDismissedCallback([this] { dismissMenu(); });
      auto menu = std::make_unique<ContextMenuControl>();
      m_menu = menu.get();
      menu->setEntries(
          {{.id = 1, .label = "Minimize"},
           {.id = 2, .label = "Maximize / Restore"},
           {.id = 3, .label = "Fullscreen"},
           {.id = 4, .label = "Close"}}
      );
      menu->setMenuWidth(240);
      menu->setOnActivate([this](const ContextMenuControlEntry& entry) {
        dismissMenu();
        switch (entry.id) {
        case 1:
          m_surface.minimize();
          break;
        case 2:
          m_surface.toggleMaximized();
          break;
        case 3:
          m_surface.toggleFullscreen();
          break;
        case 4:
          m_surface.close();
          break;
        default:
          break;
        }
      });
      m_menuRoot = ui::column(
          {.padding = 8.0F, .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh), .radius = Style::radiusLg},
          std::move(menu)
      );
      m_menuRoot->setAnimationManager(&m_animations);
      m_menuSurface->setSceneRoot(m_menuRoot.get());
      m_menuDispatcher.setSceneRoot(m_menuRoot.get());
      m_menuDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
        m_connection.input().setCursorShape(serial, shape);
      });
      m_menuSurface->setPrepareFrameCallback([this](bool, bool) {
        if (m_menuSurface == nullptr || !m_renderer.makeCurrent(m_menuSurface->renderTarget())) {
          return;
        }
        UiPhaseScope phase(UiPhase::Layout);
        auto& renderer = m_menuSurface->renderTarget().renderer();
        m_menuRoot->arrange(
            renderer,
            LayoutRect{0, 0, static_cast<float>(m_menuSurface->width()), static_cast<float>(m_menuSurface->height())}
        );
      });
      m_menuSurface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_menuSurface->requestLayout(); });
      PopupSurfaceConfig config{
          .anchorX = static_cast<int>(m_surface.width()) - 48,
          .anchorY = 48,
          .anchorWidth = 40,
          .anchorHeight = 40,
          .width = 256,
          .height = 192,
          .anchor = XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT,
          .gravity = XDG_POSITIONER_GRAVITY_BOTTOM_LEFT,
          .constraintAdjustment =
              XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y,
          .serial = m_connection.input().lastSerial(),
      };
      if (!m_menuSurface->initializeAsChild(
              m_surface.xdgSurface(), m_connection.outputForSurface(m_surface.wlSurface()), config
          )) {
        closeMenu();
      }
    }

    WaylandConnection& m_connection;
    RenderContext& m_renderer;
    TextInputService& m_textInput;
    AnimationManager m_animations;
    ToplevelSurface m_surface;
    std::unique_ptr<Node> m_root;
    InputDispatcher m_dispatcher;
    Flex* m_titlebar = nullptr;
    Input* m_location = nullptr;
    Label* m_status = nullptr;
    std::unique_ptr<PopupSurface> m_menuSurface;
    std::unique_ptr<Flex> m_menuRoot;
    ContextMenuControl* m_menu = nullptr;
    InputDispatcher m_menuDispatcher;
    std::uint64_t m_menuGeneration = 0;
    std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
  };
} // namespace

int files::runApplication() {
  initLogLevelFromEnvironment();
  initializeKeys();
  Input::setValidateKeyMatcher([](std::uint32_t sym, std::uint32_t modifiers) {
    return modifiers == 0 && KeySymbol::isEnter(sym);
  });
  g_stopRequested = 0;
  const auto previousTerm = std::signal(SIGTERM, stopRequested);
  const auto previousInt = std::signal(SIGINT, stopRequested);
  int result = EXIT_SUCCESS;
  try {
    GlibPollSource glib;
    TextInputService textInput;
    WaylandConnection connection;
    std::string error;
    if (!connection.initialize(error)) {
      throw std::runtime_error(error);
    }
    connection.bindTextInput(textInput);
    GlSharedContext graphics;
    graphics.initialize(connection.display());
    RenderContext renderer;
    renderer.initialize(graphics);
    Window window(connection, renderer, textInput);
    if (!window.initialize()) {
      throw std::runtime_error("cannot create the application window");
    }
    connection.input().setPointerEventCallback([&window](const PointerEvent& event) { window.pointer(event); });
    connection.input().setKeyboardEventCallback([&window](const KeyboardEvent& event) { window.keyboard(event); });
    connection.input().setKeyboardFocusCallback([&textInput](wl_surface* surface, bool entered) {
      textInput.onKeyboardFocusSurface(surface, entered);
    });
    connection.setOutputChangedCallback([&window](wl_output* output) { window.outputChanged(output); });
    DeferredCallPollSource deferred;
    TimerPollSource timers;
    KeyRepeatPollSource repeat(connection.input());
    MainLoop loop(connection, {&deferred, &timers, &repeat, &glib}, [&window] {
      return g_stopRequested == 0 && window.running();
    });
    loop.run();
    connection.setOutputChangedCallback({});
  } catch (const std::exception& error) {
    std::println(stderr, "noctalia-files: {}", error.what());
    result = EXIT_FAILURE;
  }
  std::signal(SIGTERM, previousTerm);
  std::signal(SIGINT, previousInt);
  return result;
}
