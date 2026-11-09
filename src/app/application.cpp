#include "app/application.h"

#include "app/file_manager_service.h"
#include "app/library_setup.h"
#include "app/main_loop.h"
#include "app/workspace_application.h"
#include "clipboard/text_clipboard.h"
#include "config/settings.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "event_loop/deferred_call_poll_source.h"
#include "event_loop/glib_poll_source.h"
#include "event_loop/timer_poll_source.h"
#include "platform/desktop_integration.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/scene/input_dispatcher.h"
#include "system/icon_theme_poll_source.h"
#include "ui/controls/archive_view.h"
#include "ui/controls/batch_rename_dialog.h"
#include "ui/controls/input.h"
#include "ui/controls/link_dialog.h"
#include "ui/controls/name_editor.h"
#include "ui/controls/permission_editor.h"
#include "wayland/text_input_service.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <array>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <format>
#include <print>
#include <string>
#include <utility>

namespace {
  void stopRequested(int) { g_stopRequested = 1; }

  class KeyRepeatPollSource final : public PollSource {
  public:
    explicit KeyRepeatPollSource(WaylandConnection& seat) : m_seat(seat) {}
    int pollTimeoutMs() const override { return m_seat.repeatPollTimeoutMs(); }
    void dispatch(const std::vector<pollfd>&, std::size_t) override { m_seat.repeatTick(); }

  private:
    void doAddPollFds(std::vector<pollfd>&) override {}
    WaylandConnection& m_seat;
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
    KeybindMatcher::setMatcher(KeybindAction::Validate, [](std::uint32_t key, std::uint32_t modifiers) {
      return modifiers == 0 && KeySymbol::isEnterOrSpace(key);
    });
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

} // namespace

int files::runApplication(const std::optional<std::string>& initialLocation, bool fileManagerActivation) {
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
    ConfigService config(ConfigPaths::fromEnvironment());
    if (!config.reload()) {
      throw std::runtime_error(config.issues().front().format());
    }
    files::initializeLibrary();
    GlibPollSource glib;
    TextInputService textInput;
    WaylandConnection connection;
    if (!connection.connect()) {
      throw std::runtime_error("could not connect to Wayland: " + connection.describeDisplayError());
    }
    connection.setTextInputService(&textInput);
    auto& desktop = connection.extension<files::DesktopIntegration>();
    WaylandTextClipboard clipboard(
        connection.display(), desktop.dataDeviceManager(), connection.seat(),
        [&connection] { return connection.lastKeyboardSurface() ? connection.lastInputSerial() : 0; },
        connection.compositor(), connection.shm()
    );
    Input::setTextClipboard(&clipboard);
    struct ClipboardBinding {
      ~ClipboardBinding() { Input::setTextClipboard(nullptr); }
    } clipboardBinding;
    GlSharedContext graphics;
    graphics.initialize(connection.display());
    RenderContext renderer;
    renderer.initialize(graphics);
    WorkspaceApplication application(connection, renderer, textInput, clipboard, config.settings().browser);
    application.imageTextures().initialize(&graphics);
    if (!fileManagerActivation) {
      application.start(initialLocation);
    }
    files::FileManagerService fileManager(
        [&application](const files::FileManagerRequest& request) { application.show(request); }, fileManagerActivation
    );
    connection.setPointerEventCallback([&application, &desktop](const PointerEvent& event) {
      desktop.pointer(event);
      application.pointer(event);
    });
    connection.setKeyboardEventCallback([&application](const KeyboardEvent& event) { application.keyboard(event); });
    connection.setKeyboardFocusCallback([&application, &clipboard](wl_surface* surface, bool entered) {
      application.focus(surface, entered);
      if (entered) {
        clipboard.keyboardFocusEntered();
      }
    });
    connection.setOutputChangeCallback([&application, &connection] {
      for (const auto& output : connection.outputs()) {
        application.outputChanged(output.output);
      }
    });
    DeferredCallPollSource deferred;
    TimerPollSource timers;
    KeyRepeatPollSource repeat(connection);
    IconThemePollSource icons;
    icons.setChangeCallback([&application] { application.iconsChanged(); });
    MainLoop loop(
        connection, {&deferred, &timers, &repeat, &glib, &icons, &application.imageTextures()},
        [&application, &fileManager] {
          const bool running = application.running();
          if (fileManager.awaitingRequest()) {
            return g_stopRequested == 0;
          }
          if (!application.hasWindows()) {
            fileManager.release();
          }
          return running;
        }
    );
    loop.run();
    connection.setOutputChangeCallback({});
    connection.setPointerEventCallback({});
    connection.setKeyboardEventCallback({});
    connection.setKeyboardFocusCallback({});
    textInput.cleanup();
    connection.setTextInputService(nullptr);
  } catch (const std::exception& error) {
    std::println(stderr, "noctalia-files: {}", error.what());
    result = EXIT_FAILURE;
  }
  std::signal(SIGTERM, previousTerm);
  std::signal(SIGINT, previousInt);
  return result;
}
