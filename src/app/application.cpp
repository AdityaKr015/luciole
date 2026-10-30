#include "app/application.h"

#include "app/deferred_call_poll_source.h"
#include "app/glib_poll_source.h"
#include "app/main_loop.h"
#include "app/timer_poll_source.h"
#include "browser/action_registry.h"
#include "browser/browser_controller.h"
#include "browser/details_columns.h"
#include "browser/file_view.h"
#include "browser/location_presentation.h"
#include "browser/path_completion.h"
#include "browser/places_sidebar.h"
#include "browser/selection_tools.h"
#include "config/settings.h"
#include "config/view_preference_writer.h"
#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/timer_manager.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "fs/filename.h"
#include "noctalia_files_build_info.h"
#include "operations/create_service.h"
#include "operations/empty_trash_service.h"
#include "operations/rename_service.h"
#include "operations/transfer_service.h"
#include "operations/trash_service.h"
#include "platform/file_launcher.h"
#include "platform/terminal_launcher.h"
#include "platform/text_clipboard.h"
#include "render/animation/animation_manager.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "system/icon_theme_poll_source.h"
#include "ui/builders.h"
#include "ui/controls/browser_settings_view.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/input.h"
#include "ui/controls/name_editor.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select_popup_context.h"
#include "ui/palette.h"
#include "wayland/popup_surface.h"
#include "wayland/text_input_service.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <format>
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

  using Action = files::BrowserAction;

  class Window : public TextInputClient, public SelectPopupContext {
  public:
    Window(
        WaylandConnection& connection, RenderContext& renderer, TextInputService& textInput,
        const files::BrowserSettings& settings, files::ConfigPaths paths, files::WaylandTextClipboard& clipboard
    )
        : m_connection(connection), m_renderer(renderer), m_textInput(textInput), m_surface(connection),
          m_browser(settings.sort, settings.showHidden),
          m_listAdapter(
              m_browser, m_icons, m_thumbnails, files::ViewMode::LIST,
              [this] { return m_connection.input().keyboardModifiers(); },
              [this] { return m_fileKeyboardFocus && m_list != nullptr && m_list->focusArea()->focused(); }
          ),
          m_gridAdapter(
              m_browser, m_icons, m_thumbnails, files::ViewMode::GRID,
              [this] { return m_connection.input().keyboardModifiers(); },
              [this] { return m_fileKeyboardFocus && m_list != nullptr && m_list->focusArea()->focused(); }
          ),
          m_preferences(std::move(paths)), m_savedSettings(settings), m_pins(files::StateStore::pathFromEnvironment()),
          m_viewMode(settings.viewMode), m_clipboard(clipboard) {
      m_gridZoom = static_cast<std::size_t>(
          std::ranges::find(files::kGridIconSizes, settings.gridIconSize) - files::kGridIconSizes.begin()
      );
      m_browser.setViewDefaults(settings.viewMode, settings.gridIconSize);
      m_thumbnails.setOptions(settings.thumbnails);
      m_thumbnails.setChangedCallback([this] {
        m_browserDirty = true;
        m_settingsDirty = true;
        m_surface.requestLayout();
      });
      m_browser.setCaptureViewport([this] {
        files::BrowseViewport viewport{.mode = m_viewMode, .iconSize = static_cast<int>(gridIconSize())};
        if (m_list) {
          viewport.vertical = m_list->scrollView().scrollOffset();
          viewport.horizontal = m_detailsOffset;
          viewport.withinRow = m_list->firstVisibleOffset();
          if (const auto first = m_list->firstVisibleIndex(); first && *first < m_browser.model().rows().size())
            viewport.first = m_browser.model().rows()[*first];
        }
        return viewport;
      });
      m_sidebarWidth = settings.sidebarWidth;
      m_showSidebar = settings.showSidebar;
      m_columnVisibility = settings.columns;
      m_columnOrder = settings.columnOrder;
      m_columnWidths = settings.columnWidths;
      m_activationMode = settings.activationMode;
      m_typingMode = settings.typingMode;
      m_surface.setRenderContext(&renderer);
      m_surface.setAnimationManager(&m_animations);
      m_surface.setConfigureCallback([this](std::uint32_t, std::uint32_t) {
        m_blankLocationClick.reset();
        if (m_dialogHeight && m_menuSurface)
          m_menuSurface->requestLayout();
        if (m_resizingSidebar || m_columnDrag || m_columnResize)
          m_dispatcher.cancelPointerCapture();
        m_surface.requestLayout();
      });
      m_surface.setPrepareFrameCallback([this](bool update, bool layout) { prepareFrame(update, layout); });
      m_surface.setUpdateCallback([] {});
      m_surface.setDebugName("files-window");
      m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
        m_connection.input().setCursorShape(serial, shape);
      });
      m_dispatcher.setFocusChangeCallback([this](InputArea*, InputArea* focused) {
        if (m_settingsView) {
          m_settingsView->reveal(focused);
          m_surface.requestLayout();
        }
        if (m_list != nullptr && focused != m_list->focusArea())
          clearTypeahead();
        if (m_nameHeader == nullptr || m_viewMode != files::ViewMode::LIST)
          return;
        for (std::size_t index = 0; index < 4; ++index) {
          if (widthHeader(index)->inputArea() == focused) {
            m_revealDetailsHeader = widthHeader(index);
            m_surface.requestLayout();
          }
        }
      });
      m_browser.setChangedCallback([this] {
        m_browserDirty = true;
        m_surface.requestUpdate();
      });
      m_browser.setFileActivatedCallback([this](const files::Location& location) { m_launcher.open(location); });
      m_launcher.setChangedCallback([this](const std::string& error) {
        m_actionError = error.empty() ? "" : std::format("Could not open the file: {}", error);
        if (m_aboutMessage) {
          m_aboutMessage->setText(error.empty() ? "" : std::format("Could not open the link: {}", error));
          m_aboutMessage->setVisible(!error.empty());
          m_aboutMessage->setParticipatesInLayout(!error.empty());
          m_menuSurface->requestLayout();
        }
        m_browserDirty = true;
        m_surface.requestUpdate();
      });
      m_transfers.setChangedCallback([this] {
        updateOperationProgress();
        if (!m_transfers.busy()) {
          const auto& results = m_transfers.results();
          const auto done = std::ranges::count_if(results, [](const auto& item) {
            return item.outcome == files::TransferOutcome::MOVED || item.outcome == files::TransferOutcome::COPIED;
          });
          m_actionError = std::format(
              "{} item{} {}", done, done == 1 ? "" : "s",
              m_transfers.action() == files::TransferAction::COPY ? "copied" : "moved"
          );
          if (const auto error = std::ranges::find_if(results, [](const auto& item) { return !item.error.empty(); });
              error != results.end()) {
            const files::GString raw(g_file_get_basename(error->source.file().get()));
            const files::GString name(g_filename_display_name(raw ? raw.get() : "Item"));
            m_actionError += std::format(" · {}: {}", name.get(), error->error);
          }
          m_browser.refresh();
        }
        m_browserDirty = true;
        m_surface.requestUpdate();
      });
      m_emptyTrash.setChangedCallback([this] {
        updateOperationProgress();
        m_emptyTrashDirty = true;
        m_browserDirty = true;
        m_surface.requestUpdate();
      });
      m_trash.setChangedCallback([this] {
        updateOperationProgress();
        m_trashDirty = true;
        m_browserDirty = true;
        m_surface.requestUpdate();
      });
      m_creator.setChangedCallback([this] {
        m_creationDirty = true;
        m_browserDirty = true;
        m_surface.requestUpdate();
      });
      m_renamer.setChangedCallback([this] {
        m_renameDirty = true;
        m_browserDirty = true;
        m_surface.requestUpdate();
      });
      m_pins.setChangedCallback([this] {
        m_placesDirty = true;
        m_browserDirty = true;
        m_surface.requestUpdate();
      });
      for (auto* adapter : {&m_listAdapter, &m_gridAdapter}) {
        adapter->setActivationMode(m_activationMode);
        adapter->setActivateCallback([this](files::EntryId id) {
          auto request = actionRequest(false);
          if (const auto* entry = m_browser.model().entry(id))
            request.targets.push_back({id, entry->location});
          dispatchAction(Action::OPEN, request);
        });
        adapter->setContextCallback([this](std::optional<files::EntryId> id, float x, float y) {
          openFileMenu(id, x, y);
        });
        adapter->setDragCallback(
            [this](const std::vector<files::EntryId>& ids) { startFileDrag(ids); },
            [this] {
              if (m_drag && !m_drag->sourcePin) {
                cancelDrag();
              }
            }
        );
      }
      m_icons.setChangedCallback([this] {
        m_browserDirty = true;
        m_surface.requestUpdate();
        if (m_menuSurface != nullptr && m_menu == nullptr) {
          m_menuSurface->requestLayout();
        }
      });
      m_preferences.setChangedCallback([this](const std::string& error) {
        m_preferenceError = error;
        m_pendingSettings = m_preferences.settings();
        m_settingsDirty = true;
        m_browserDirty = true;
        m_surface.requestUpdate();
      });
      m_preferences.startWatching();
    }

    ~Window() override {
      m_dragScroll.stop();
      m_operationProgress.stop();
      m_transfers.setChangedCallback({});
      m_trash.setChangedCallback({});
      m_emptyTrash.setChangedCallback({});
      m_renamer.setChangedCallback({});
      m_creator.setChangedCallback({});
      m_browser.setChangedCallback({});
      m_browser.setFileActivatedCallback({});
      m_launcher.setChangedCallback({});
      m_pins.setChangedCallback({});
      m_icons.setChangedCallback({});
      m_thumbnails.setChangedCallback({});
      m_preferences.setChangedCallback({});
      closeMenu();
      closeSettings();
      m_dispatcher.setSceneRoot(nullptr);
      m_surface.setSceneRoot(nullptr);
      m_root.reset();
    }

    void iconThemeChanged() {
      m_icons.invalidate();
      m_browserDirty = true;
      m_surface.requestUpdate();
    }

    bool initialize(const std::optional<std::string>& initialLocation) {
      ToplevelSurfaceConfig config{
          .width = 1080,
          .height = 720,
          .minWidth = 620,
          .minHeight = 400,
          .title = "Noctalia Files",
          .appId = "dev.noctalia.Files",
      };
      if (!m_surface.initialize(nullptr, config)) {
        return false;
      }
      m_pins.reload();
      if (initialLocation) {
        (void)m_browser.openText(*initialLocation);
      } else {
        m_browser.home();
      }
      return true;
    }

    [[nodiscard]] bool running() {
      if (!m_surface.isRunning() && m_transfers.busy()) {
        m_transfers.cancel();
      }
      if (!m_surface.isRunning() && m_emptyTrash.pending())
        m_emptyTrash.cancel();
      if (!m_surface.isRunning() && (m_trash.busy() || m_trash.canTrash()))
        m_trash.cancel();
      if (!m_surface.isRunning() && m_renamer.busy())
        m_renamer.cancel();
      if (!m_surface.isRunning() && m_creator.busy())
        m_creator.cancel();
      return m_surface.isRunning()
          || m_preferences.busy()
          || m_pins.busy()
          || m_launcher.busy()
          || m_transfers.busy()
          || m_trash.busy()
          || m_emptyTrash.busy()
          || m_renamer.busy()
          || m_creator.busy();
    }

    void outputChanged(wl_output* output) {
      if (m_connection.outputForSurface(m_surface.wlSurface()) == output) {
        m_surface.onSurfaceOutputEnter(m_surface.wlSurface(), output);
      }
      if (m_menuSurface != nullptr && m_connection.outputForSurface(m_menuSurface->wlSurface()) == output) {
        m_menuSurface->onSurfaceOutputEnter(m_menuSurface->wlSurface(), output);
      }
    }

    void pointer(const PointerEvent& event) {
      if (m_menuSurface != nullptr && event.type == PointerEvent::Type::Button && event.pressed) {
        const bool inPopup = event.surface == m_menuSurface->wlSurface()
            && event.sx >= 0
            && event.sy >= 0
            && event.sx < m_menuSurface->width()
            && event.sy < m_menuSurface->height();
        if (!inPopup) {
          m_dispatcher.cancelPointerCapture();
          dismissMenu();
          return;
        }
      }
      auto* dispatcher = event.surface == m_surface.wlSurface()                     ? &m_dispatcher
          : m_menuSurface != nullptr && event.surface == m_menuSurface->wlSurface() ? &m_menuDispatcher
                                                                                    : nullptr;
      if (dispatcher == nullptr) {
        return;
      }
      const auto x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
      if (m_settingsView && dispatcher == &m_dispatcher) {
        m_pointerX = x;
        m_pointerY = y;
        if (event.type == PointerEvent::Type::Button && event.pressed)
          m_popupSerial = event.serial;
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
        m_surface.requestUpdate();
        return;
      }
      m_pointerX = x;
      m_pointerY = y;
      if (dispatcher == &m_dispatcher
          && m_blankLocationClick
          && (event.type == PointerEvent::Type::Leave
              || event.type == PointerEvent::Type::Axis
              || (event.type == PointerEvent::Type::Motion
                  && (std::abs(x - m_blankLocationClick->x) > 6.0F || std::abs(y - m_blankLocationClick->y) > 6.0F))))
        m_blankLocationClick.reset();
      if (dispatcher == &m_dispatcher
          && event.type == PointerEvent::Type::Button
          && event.pressed
          && m_editingLocation
          && m_completionFrame
          && !m_completionFrame->containsScenePoint(x, y))
        clearCompletions();
      if (event.type == PointerEvent::Type::Button && event.pressed && dispatcher == &m_dispatcher)
        clearTypeahead();
      if (event.type == PointerEvent::Type::Button && event.pressed) {
        m_popupSerial = event.serial;
        if (dispatcher == &m_dispatcher) {
          m_fileKeyboardFocus = false;
          m_browserDirty = true;
          m_surface.requestUpdate();
        }
      }
      if (dispatcher == &m_dispatcher
          && m_columnDrag
          && m_columnDrag->active
          && event.type == PointerEvent::Type::Button
          && event.button == BTN_LEFT
          && !event.pressed) {
        finishColumnDrag(x, y);
        return;
      }
      if (dispatcher == &m_dispatcher
          && event.type == PointerEvent::Type::Button
          && event.pressed
          && event.button == BTN_RIGHT
          && m_listHeader->visible()
          && m_listHeader->containsScenePoint(x, y)) {
        std::optional<files::DetailsColumn> column;
        for (const auto candidate : m_columnOrder) {
          auto* header = columnHeader(candidate);
          if (header->visible() && header->containsScenePoint(x, y))
            column = candidate;
        }
        openColumnsMenu({x, y, 1, 1}, column);
        return;
      }
      if (dispatcher == &m_dispatcher
          && event.type == PointerEvent::Type::Button
          && event.pressed
          && event.button == BTN_RIGHT
          && m_notice != nullptr
          && m_noticeArea->visible()
          && m_filePane->containsScenePoint(x, y)
          && (!m_listHeader->visible() || !m_listHeader->containsScenePoint(x, y))) {
        openFileMenu(std::nullopt, x, y);
        return;
      }
      if (dispatcher == &m_dispatcher
          && m_drag
          && event.type == PointerEvent::Type::Button
          && event.button == BTN_LEFT
          && !event.pressed) {
        finishDrag(x, y);
        return;
      }
      if (dispatcher == &m_dispatcher && event.type == PointerEvent::Type::Button && event.pressed) {
        const bool twice = event.button == BTN_LEFT
            && m_lastBreadcrumb
            && !m_editingLocation
            && m_browser.model().snapshot().location == m_lastBreadcrumb
            && std::chrono::steady_clock::now() - m_breadcrumbClickTime < std::chrono::milliseconds(400)
            && std::abs(x - m_breadcrumbClickX) <= 6.0F
            && std::abs(y - m_breadcrumbClickY) <= 6.0F;
        m_lastBreadcrumb.reset();
        float localX = 0, localY = 0;
        if (twice
            && m_locationArea != nullptr
            && Node::mapFromScene(m_locationArea, x, y, localX, localY)
            && localX >= 0
            && localY >= 0
            && localX < m_locationArea->width()
            && localY < m_locationArea->height()) {
          // Recognize the second press across the ancestor navigation's scene rebuild.
          m_breadcrumbEditTarget = m_browser.model().snapshot().location;
          m_browserDirty = true;
          m_surface.requestUpdate();
          return;
        }
      }
      if (dispatcher == &m_dispatcher && event.type == PointerEvent::Type::Button && event.pressed) {
        const auto& location = m_browser.model().snapshot().location;
        const bool blank = event.button == BTN_LEFT
            && m_connection.input().keyboardModifiers() == 0
            && !m_editingLocation
            && location
            && m_locationArea
            && m_locationArea->containsScenePoint(x, y)
            && std::ranges::none_of(m_dropBreadcrumbs, [x, y](const auto& crumb) {
                             return crumb.second->containsScenePoint(x, y);
                           });
        if (blank) {
          const auto now = std::chrono::steady_clock::now();
          const bool twice = m_blankLocationClick
              && m_blankLocationClick->location == location
              && m_blankLocationClick->context == m_browser.contextSerial()
              && now - m_blankLocationClick->time < std::chrono::milliseconds(400)
              && std::abs(x - m_blankLocationClick->x) <= 6.0F
              && std::abs(y - m_blankLocationClick->y) <= 6.0F;
          if (twice) {
            m_blankLocationClick.reset();
            m_breadcrumbEditTarget = location;
            m_surface.requestUpdate();
            return;
          }
          m_blankLocationClick = LocationBlankClick{*location, m_browser.contextSerial(), now, x, y};
        } else
          m_blankLocationClick.reset();
      }
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
        if (dispatcher == &m_dispatcher) {
          cancelDrag();
        }
        break;
      case PointerEvent::Type::Motion:
        dispatcher->pointerMotion(x, y, event.serial);
        if (dispatcher == &m_dispatcher && m_drag) {
          updateDrag();
        }
        break;
      case PointerEvent::Type::Button:
        dispatcher->pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
        break;
      case PointerEvent::Type::Axis:
        if (dispatcher == &m_dispatcher
            && m_viewMode == files::ViewMode::LIST
            && m_filePane->containsScenePoint(x, y)
            && (event.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL
                || (event.axis == WL_POINTER_AXIS_VERTICAL_SCROLL
                    && m_connection.input().keyboardModifiers() == KeyMod::Shift))) {
          if (!m_columnResize && !m_columnDrag) {
            InputArea::PointerData data;
            data.axisValue = event.axisValue;
            data.axisDiscrete = event.axisDiscrete;
            data.axisValue120 = event.axisValue120;
            data.axisLines = event.axisLines;
            scrollDetails(m_detailsOffset + data.scrollDelta(Style::scrollWheelStep));
          }
          break;
        }
        if (dispatcher == &m_dispatcher && zoomFromScroll(event)) {
          break;
        }
        dispatcher->pointerAxis(
            x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120,
            event.axisLines, event.axisGestureSerial
        );
        break;
      }
    }

    void keyboard(const KeyboardEvent& event) {
      if (event.pressed) {
        m_popupSerial = m_connection.input().lastSerial();
        m_lastBreadcrumb.reset();
        m_blankLocationClick.reset();
        if ((m_columnDrag || m_columnResize) && KeySymbol::isEscape(event.sym)) {
          m_dispatcher.cancelPointerCapture();
          return;
        }
        if (m_resizingSidebar && KeySymbol::isEscape(event.sym)) {
          m_dispatcher.cancelPointerCapture();
          return;
        }
        if (m_drag && KeySymbol::isEscape(event.sym)) {
          cancelDrag();
          return;
        }
      }
      if (event.pressed
          && event.modifiers == 0
          && KeySymbol::isEscape(event.sym)
          && (!m_typeahead.text().empty() || !m_typeaheadPreedit.empty())) {
        clearTypeahead();
        return;
      }
      if (m_columnDrag || m_columnResize)
        return;
      if (m_drag) {
        updateDrag();
        return;
      }
      if (m_menuSurface != nullptr) {
        if (m_menu == nullptr) {
          if (event.pressed && KeySymbol::isEscape(event.sym)) {
            dismissMenu();
          } else if (event.pressed && KeySymbol::isTab(event.sym)) {
            (void)m_menuDispatcher.cycleTabFocus((event.modifiers & KeyMod::Shift) != 0);
          } else {
            m_menuDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
          }
          return;
        }
        if (!event.pressed) {
          return;
        }
        if (KeySymbol::isEscape(event.sym)) {
          dismissMenu();
        } else if (KeySymbol::isUp(event.sym)) {
          if (m_menu->moveHighlight(-1)) {
            revealMenuHighlight();
            m_menuSurface->requestRedraw();
          }
        } else if (KeySymbol::isDown(event.sym)) {
          if (m_menu->moveHighlight(1)) {
            revealMenuHighlight();
            m_menuSurface->requestRedraw();
          }
        } else if (KeySymbol::isEnter(event.sym)) {
          (void)m_menu->activateHighlighted();
        }
        return;
      }
      if (m_settingsView) {
        if (event.pressed
            && (KeySymbol::isEscape(event.sym) || (event.sym == XKB_KEY_comma && event.modifiers == KeyMod::Ctrl)))
          deferCloseSettings();
        else if (event.pressed && event.sym == XKB_KEY_q && event.modifiers == KeyMod::Ctrl)
          m_surface.close();
        else if (event.pressed && KeySymbol::isTab(event.sym))
          (void)m_dispatcher.cycleTabFocusInSubtree(m_settingsView, (event.modifiers & KeyMod::Shift) != 0);
        else
          m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
        m_surface.requestLayout();
        return;
      }
      if (event.pressed && !event.preedit) {
        const bool editing = (m_location != nullptr && m_dispatcher.focusedArea() == m_location->inputArea())
            || (m_filterInput != nullptr && m_dispatcher.focusedArea() == m_filterInput->inputArea());
        if (!m_typeaheadPreedit.empty()) {
          if (KeySymbol::isEscape(event.sym))
            clearTypeahead();
          return;
        }
        if ((event.sym == XKB_KEY_Menu && event.modifiers == 0)
            || (event.sym == XKB_KEY_F10 && event.modifiers == KeyMod::Shift)) {
          for (const auto& [target, button] : m_dropBreadcrumbs)
            if (m_dispatcher.focusedArea() == button->inputArea()) {
              openBreadcrumbMenu(target, sceneRect(*button));
              return;
            }
        }
        if (m_editingLocation && completionKey(event.sym, event.modifiers))
          return;
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
        if (event.sym == XKB_KEY_comma && event.modifiers == KeyMod::Ctrl) {
          dispatchAction(Action::SETTINGS);
          return;
        }
        if (event.sym == XKB_KEY_l && event.modifiers == KeyMod::Ctrl && m_location != nullptr) {
          dispatchAction(Action::EDIT_LOCATION);
          return;
        }
        if ((event.sym == XKB_KEY_k || event.sym == XKB_KEY_f)
            && event.modifiers == KeyMod::Ctrl
            && m_filterInput != nullptr) {
          dispatchAction(Action::SEARCH);
          return;
        }
        if (m_editingLocation && event.modifiers == 0 && KeySymbol::isEscape(event.sym)) {
          endLocationEdit(true);
          return;
        }
        if (m_filterOpen && event.modifiers == 0 && KeySymbol::isEscape(event.sym) && !m_editingLocation) {
          closeFilter();
          return;
        }
        if (!editing && event.modifiers == KeyMod::Alt && event.sym == XKB_KEY_Left) {
          dispatchAction(Action::BACK);
          return;
        }
        if (!editing && event.modifiers == KeyMod::Alt && event.sym == XKB_KEY_Right) {
          dispatchAction(Action::FORWARD);
          return;
        }
        if (!editing && event.modifiers == KeyMod::Alt && event.sym == XKB_KEY_Up) {
          dispatchAction(Action::UP);
          return;
        }
        if (!editing && event.modifiers == KeyMod::Alt && event.sym == XKB_KEY_Home) {
          dispatchAction(Action::HOME);
          return;
        }
        if (!editing && event.sym == XKB_KEY_F5 && event.modifiers == 0) {
          dispatchAction(Action::REFRESH);
          return;
        }
        if (!editing
            && m_viewMode == files::ViewMode::LIST
            && event.modifiers == KeyMod::Ctrl
            && (event.sym == XKB_KEY_Left || event.sym == XKB_KEY_Right)) {
          for (std::size_t index = 0; index < 4; ++index) {
            if (m_dispatcher.focusedArea() == widthHeader(index)->inputArea()) {
              auto widths = m_columnWidths;
              const int delta = (event.sym == XKB_KEY_Right ? 16 : -16) * (Style::rtl() ? -1 : 1);
              widths[index] = std::clamp(
                  static_cast<int>(std::lround(headerWidth(index))) + delta, files::minimumDetailsColumnWidth(index),
                  files::maximumDetailsColumnWidth(index)
              );
              if (index != 0 && widths[0] == 0 && widths[index] != m_columnWidths[index])
                widths[0] = static_cast<int>(std::lround(m_detailsColumns.nameWidth));
              setDetailsColumnWidths(widths);
              m_revealDetailsHeader = widthHeader(index);
              return;
            }
          }
        }
        if (!editing && zoomFromKey(event))
          return;
        if (!editing && event.sym == XKB_KEY_F9 && event.modifiers == 0 && !event.repeat) {
          dispatchAction(Action::SHOW_SIDEBAR);
          return;
        }
        if (!editing
            && (event.sym == XKB_KEY_n || event.sym == XKB_KEY_N)
            && event.modifiers == (KeyMod::Ctrl | KeyMod::Shift)
            && !event.repeat) {
          dispatchAction(Action::CREATE_FOLDER);
          return;
        }
        if (!editing
            && ((event.sym == XKB_KEY_Menu && event.modifiers == 0)
                || (event.sym == XKB_KEY_F10 && event.modifiers == KeyMod::Shift))) {
          for (auto* header : {m_nameHeader, m_typeHeader, m_sizeHeader, m_modifiedHeader}) {
            if (m_dispatcher.focusedArea() == header->inputArea()) {
              std::optional<files::DetailsColumn> column;
              for (const auto candidate : m_columnOrder)
                if (columnHeader(candidate) == header)
                  column = candidate;
              auto anchor = sceneRect(*header);
              const auto viewport = sceneRect(*m_detailsHeaderViewport);
              const float left = std::max(anchor.x, viewport.x);
              anchor.width = std::max(1.0F, std::min(anchor.x + anchor.width, viewport.x + viewport.width) - left);
              anchor.x = left;
              openColumnsMenu(anchor, column);
              return;
            }
          }
        }
        if (!editing
            && m_list != nullptr
            && m_dispatcher.focusedArea() == m_list->focusArea()
            && (event.sym == XKB_KEY_Menu || (event.sym == XKB_KEY_F10 && event.modifiers == KeyMod::Shift))) {
          auto id = m_browser.model().selection().focused;
          float x = 240, y = 100;
          if (id) {
            if (!m_browser.model().selection().selected.contains(*id)) {
              m_browser.model().select(*id);
            }
            const auto& rows = m_browser.model().rows();
            const auto found = std::ranges::find(rows, *id);
            if (found != rows.end()) {
              if (const auto rect = m_list->itemRect(static_cast<std::size_t>(found - rows.begin()))) {
                x = rect->x + rect->width * 0.5F;
                y = rect->y + rect->height * 0.5F;
              }
            }
          }
          openFileMenu(id, x, y);
          return;
        }
        if (!editing && m_list != nullptr && m_dispatcher.focusedArea() == m_list->focusArea() && !event.repeat) {
          if (event.sym == XKB_KEY_Delete && event.modifiers == 0) {
            dispatchAction(Action::TRASH);
            return;
          }
          if (event.sym == XKB_KEY_F2 && event.modifiers == 0) {
            dispatchAction(Action::RENAME);
            return;
          }
          if (event.sym == XKB_KEY_Return && event.modifiers == KeyMod::Alt) {
            dispatchAction(Action::PROPERTIES);
            return;
          }
        }
        if (!editing && event.modifiers == KeyMod::Ctrl && (event.sym == XKB_KEY_1 || event.sym == XKB_KEY_2)) {
          dispatchAction(event.sym == XKB_KEY_1 ? Action::GRID : Action::LIST);
          return;
        }
        if (!editing && event.sym == XKB_KEY_h && event.modifiers == KeyMod::Ctrl) {
          dispatchAction(Action::SHOW_HIDDEN);
          return;
        }
        if (m_list != nullptr && m_dispatcher.focusedArea() == m_list->focusArea() && listKey(event)) {
          m_fileKeyboardFocus = !KeySymbol::isEscape(event.sym);
          m_browserDirty = true;
          m_surface.requestUpdate();
          return;
        }
      }
      if (event.pressed
          && !event.preedit
          && m_typingMode == files::TypingMode::TYPEAHEAD
          && m_list != nullptr
          && m_dispatcher.focusedArea() == m_list->focusArea()
          && (event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) == 0) {
        if (g_unichar_isprint(event.utf32)) {
          char text[8]{};
          const int length = g_unichar_to_utf8(event.utf32, text);
          applyTypeahead(std::string(text, static_cast<std::size_t>(length)));
          return;
        }
        if (event.sym == XKB_KEY_BackSpace && !m_typeahead.text().empty()) {
          const auto& text = m_typeahead.text();
          const char* previous = g_utf8_find_prev_char(text.data(), text.data() + text.size());
          typeaheadChanged(m_typeahead.erase(
              m_browser.model(), static_cast<std::uint32_t>(text.data() + text.size() - previous),
              files::TypeaheadSelection::Clock::now()
          ));
          return;
        }
      }
      if (event.pressed
          && !event.preedit
          && m_typingMode == files::TypingMode::SEARCH
          && m_list != nullptr
          && m_dispatcher.focusedArea() == m_list->focusArea()
          && (event.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) == 0
          && (g_unichar_isprint(event.utf32) || (m_filterOpen && event.sym == XKB_KEY_BackSpace))) {
        openFilter(false);
      }
      m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    }

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

    files::ActionContext actionContext() const {
      return {
          .model = m_browser.model(),
          .context = m_browser.contextSerial(),
          .query = m_browser.searchQuery(),
          .canBack = m_browser.canBack(),
          .canForward = m_browser.canForward(),
          .canUp = m_browser.canUp(),
          .operationsBusy = m_creator.busy()
              || m_renamer.busy()
              || m_transfers.busy()
              || m_trash.busy()
              || m_trash.canTrash()
              || m_emptyTrash.pending(),
          .pinsEditable = m_pins.editable(),
          .pinsBusy = m_pins.busy(),
          .canEmptyTrash = canEmptyTrash(),
          .pinned = [this](const auto& location) { return m_pins.contains(location); },
          .viewMode = m_viewMode,
          .showSidebar = m_showSidebar,
          .clipboardAvailable = m_clipboard.available(),
          .terminalAvailable = m_terminal.available(),
          .activationMode = m_activationMode,
          .typingMode = m_typingMode
      };
    }
    files::ActionRequest actionRequest(bool withTargets = true) const {
      if (withTargets)
        return files::ActionRegistry::capture(actionContext());
      return {m_browser.contextSerial(), m_browser.model().snapshot().location, m_browser.searchQuery(), {}};
    }
    files::ActionRequest requestFor(const std::vector<files::DirectoryEntry>& entries) const {
      auto request = actionRequest(false);
      for (const auto& entry : entries)
        request.targets.push_back({entry.id, entry.location});
      return request;
    }
    files::ActionState actionState(Action action, const files::ActionRequest& request) const {
      return files::ActionRegistry::state(action, actionContext(), request);
    }
    ContextMenuControlEntry actionEntry(Action action, const files::ActionRequest& request, bool radio = false) const {
      const auto& definition = files::ActionRegistry::definition(action);
      const auto state = actionState(action, request);
      return {
          .id = static_cast<int>(action),
          .label = std::string(definition.label),
          .enabled = state.enabled,
          .destructive = definition.destructive,
          .checkmark = !radio && state.checked != -1,
          .radio = radio,
          .toggleState = state.checked,
          .shortcut = definition.shortcut.empty() ? std::nullopt : std::optional(std::string(definition.shortcut))
      };
    }
    void dispatchAction(Action action) {
      dispatchAction(
          action, actionRequest(files::ActionRegistry::definition(action).scope == files::ActionScope::TARGETS)
      );
    }
    void dispatchAction(Action action, const files::ActionRequest& request) {
      if (action == Action::OPEN)
        Logger("file-launch").debug("Open action dispatch ({} targets)", request.targets.size());
      const auto state = actionState(action, request);
      if (!state.enabled) {
        actionError(std::string(state.reason));
        return;
      }
      m_actionError.clear();
      clearTypeahead();
      const bool selectionAction = files::ActionRegistry::definition(action).scope == files::ActionScope::SELECTION;
      if (selectionAction) {
        m_dispatcher.cancelPointerCapture();
        m_list->cancelPointerGesture();
      }
      std::vector<files::DirectoryEntry> targets;
      for (const auto& target : request.targets)
        if (const auto* entry = m_browser.model().entry(target.id))
          targets.push_back(*entry);
      switch (action) {
      case Action::COPY_PATH:
      case Action::COPY_URI:
      case Action::COPY_TARGET_PATH:
      case Action::COPY_TARGET_URI: {
        std::string text;
        const bool uri = action == Action::COPY_URI || action == Action::COPY_TARGET_URI;
        const auto append = [&](const files::Location& location) {
          if (!text.empty())
            text += '\n';
          text += uri ? location.uri() : files::locationEditText(location);
        };
        if (action == Action::COPY_PATH || action == Action::COPY_URI)
          append(*request.folder);
        else
          for (const auto& target : targets)
            append(target.location);
        if (text.size() > 1024 * 1024)
          actionError("The selected paths exceed the clipboard text limit.");
        else
          m_clipboard.setClipboardText(std::move(text));
        break;
      }
      case Action::TERMINAL:
      case Action::TARGET_TERMINAL: {
        const auto target = action == Action::TERMINAL ? *request.folder : targets.front().location;
        const auto context = m_browser.contextSerial();
        const std::weak_ptr<int> lifetime = m_lifetime;
        m_terminal.open(target, [this, lifetime, context](std::string error) {
          if (!lifetime.expired() && context == m_browser.contextSerial() && !error.empty())
            actionError(std::move(error));
        });
        break;
      }
      case Action::BACK:
        (void)m_browser.back();
        break;
      case Action::FORWARD:
        (void)m_browser.forward();
        break;
      case Action::UP:
        (void)m_browser.up();
        break;
      case Action::HOME:
        m_browser.home();
        break;
      case Action::ROOT:
        m_browser.open(files::Location::fromPath("/"));
        break;
      case Action::REFRESH:
        m_browser.refresh();
        break;
      case Action::STOP:
        m_browser.stop();
        break;
      case Action::EDIT_LOCATION:
        beginLocationEdit();
        break;
      case Action::SEARCH:
        openFilter();
        break;
      case Action::GRID:
        setViewMode(files::ViewMode::GRID);
        break;
      case Action::LIST:
        setViewMode(files::ViewMode::LIST);
        break;
      case Action::SHOW_HIDDEN:
        toggleHidden();
        break;
      case Action::SHOW_SIDEBAR:
        setSidebarVisible(!m_showSidebar);
        break;
      case Action::SORT_NAME:
      case Action::SORT_TYPE:
      case Action::SORT_SIZE:
      case Action::SORT_MODIFIED:
      case Action::ASCENDING:
      case Action::DESCENDING:
      case Action::FOLDERS_FIRST: {
        auto options = m_browser.model().sort();
        const auto field = action == Action::SORT_NAME ? files::SortField::NAME
            : action == Action::SORT_TYPE              ? files::SortField::TYPE
            : action == Action::SORT_SIZE              ? files::SortField::SIZE
                                                       : files::SortField::MODIFIED;
        if (action == Action::ASCENDING || action == Action::DESCENDING)
          options.descending = action == Action::DESCENDING;
        else if (action == Action::FOLDERS_FIRST)
          options.directoriesFirst = !options.directoriesFirst;
        else if (options.field != field) {
          options.field = field;
          options.descending = false;
        }
        changeSort(options);
        break;
      }
      case Action::OPEN:
        for (const auto& target : targets)
          m_browser.activate(target.id);
        break;
      case Action::CONTAINING_FOLDER:
        showContaining(targets.front().location);
        break;
      case Action::ORIGINAL_LOCATION:
        showContaining(*targets.front().trash.original);
        break;
      case Action::PIN: {
        std::vector<files::Bookmark> pins;
        for (const auto& target : targets)
          if (!m_pins.contains(target.location))
            pins.push_back({target.displayName, target.location});
        pinFolders(std::move(pins), m_pins.bookmarks().size());
        break;
      }
      case Action::UNPIN: {
        std::vector<files::Location> locations;
        locations.reserve(targets.size());
        for (const auto& target : targets)
          locations.push_back(target.location);
        m_pins.removeMany(locations);
        break;
      }
      case Action::PIN_CURRENT:
        pinFolders(
            {{files::locationBreadcrumbs(*request.folder).back().label, *request.folder}}, m_pins.bookmarks().size()
        );
        break;
      case Action::UNPIN_CURRENT:
        m_pins.remove(*request.folder);
        break;
      case Action::RELOAD_PINS:
        m_pins.reload();
        break;
      case Action::EMPTY_TRASH:
        openEmptyTrash();
        break;
      case Action::RENAME:
        openFileRename(targets.front());
        break;
      case Action::TRASH:
        trashItems(targets);
        break;
      case Action::PROPERTIES:
        showProperties(targets);
        break;
      case Action::CREATE_FOLDER:
      case Action::CREATE_FILE:
        openCreateItem(
            *request.folder, action == Action::CREATE_FOLDER ? files::CreateKind::FOLDER : files::CreateKind::EMPTY_FILE
        );
        break;
      case Action::SELECT_ALL:
        m_browser.model().selectAll();
        break;
      case Action::INVERT_SELECTION:
        (void)m_browser.model().invertSelection();
        break;
      case Action::CLEAR_SELECTION:
        m_list->cancelPointerGesture();
        m_browser.model().clearSelection();
        break;
      case Action::SELECT_PATTERN:
        openPatternSelection(request);
        break;
      case Action::SINGLE_CLICK:
      case Action::DOUBLE_CLICK:
      case Action::TYPE_TO_SEARCH:
      case Action::TYPEAHEAD:
        if (action == Action::SINGLE_CLICK || action == Action::DOUBLE_CLICK)
          m_activationMode = action == Action::SINGLE_CLICK ? files::ActivationMode::SINGLE_CLICK
                                                            : files::ActivationMode::DOUBLE_CLICK;
        else
          m_typingMode = action == Action::TYPEAHEAD ? files::TypingMode::TYPEAHEAD : files::TypingMode::SEARCH;
        for (auto* adapter : {&m_listAdapter, &m_gridAdapter}) {
          adapter->onPointerCancel();
          adapter->setActivationMode(m_activationMode);
        }
        if (state.checked != 1)
          m_preferences.saveInteraction(m_activationMode, m_typingMode);
        break;
      case Action::SETTINGS:
        openSettings();
        break;
      case Action::ABOUT:
        openAbout();
        break;
      case Action::VIEW_MENU:
        openMenu();
        break;
      case Action::SORT_MENU:
        openSortMenu();
        break;
      case Action::SELECTION_MENU:
        openSelectionMenu();
        break;
      case Action::INTERACTION_MENU:
        openInteractionMenu();
        break;
      }
      if (!m_menuSurface
          && (selectionAction
              || action == Action::SINGLE_CLICK
              || action == Action::DOUBLE_CLICK
              || action == Action::TYPE_TO_SEARCH
              || action == Action::TYPEAHEAD))
        m_dispatcher.setFocus(m_list->focusArea());
      m_browserDirty = true;
      m_surface.requestUpdate();
    }
    void openPatternSelection(const files::ActionRequest& request) {
      const float width = std::min(440.0F, static_cast<float>(m_surface.width()) - 32.0F);
      auto editor = std::make_unique<NameEditor>(
          width, "Select by pattern", m_selectionPattern,
          "Use * for any text and ? for one character. Matches visible names, ignoring case.",
          files::selectionPatternProblem,
          [this, request](std::string pattern) {
            const auto state = actionState(Action::SELECT_PATTERN, request);
            if (!state.enabled) {
              m_patternEditor->showError(std::string(state.reason));
              return;
            }
            if (const auto error = m_browser.model().selectPattern(pattern)) {
              m_patternEditor->showError(*error);
              return;
            }
            m_selectionPattern = std::move(pattern);
            dismissMenu();
            m_dispatcher.setFocus(m_list->focusArea());
          },
          [this] { dismissMenu(); }, "Select", "Selecting…"
      );
      auto* control = editor.get();
      control->input()->setPlaceholder("Pattern");
      UiPhaseScope phase(UiPhase::Layout);
      LayoutConstraints constraints;
      constraints.setExactWidth(width);
      const float height = editor->measure(m_surface.renderTarget().renderer(), constraints).height;
      showPopup(
          std::move(editor), nullptr, {(m_surface.width() - width) * 0.5F, (m_surface.height() - height) * 0.5F, 1, 1},
          width, height
      );
      if (m_menuSurface) {
        m_patternEditor = control;
        m_menuDispatcher.setFocus(control->input()->inputArea());
        control->input()->selectAll();
      }
    }

    void actionError(std::string message) {
      m_actionError = std::move(message);
      m_browserDirty = true;
      m_surface.requestUpdate();
    }

    bool targetsCurrent(const std::vector<files::DirectoryEntry>& targets) {
      const bool current = std::ranges::all_of(targets, [this](const auto& target) {
        const auto* entry = m_browser.model().entry(target.id);
        return entry != nullptr
            && entry->location == target.location
            && std::ranges::contains(m_browser.model().rows(), target.id);
      });
      if (!current) {
        actionError("The selection changed. Open the menu again.");
      }
      return current;
    }

    void pinFolders(std::vector<files::Bookmark> bookmarks, std::size_t before) {
      if (!m_pins.add(std::move(bookmarks), before)) {
        actionError(
            m_pins.busy() ? "Pins are being saved. Try again shortly." : "Reload valid pins before editing them."
        );
      }
    }

    void openFileMenu(std::optional<files::EntryId> id, float x, float y) {
      m_actionError.clear();
      const auto request = actionRequest(id.has_value());
      std::vector<ContextMenuControlEntry> entries;
      const auto add = [&](Action action) { entries.push_back(actionEntry(action, request)); };
      const auto separator = [&] { entries.push_back({.label = "", .separator = true}); };
      if (!id) {
        add(Action::CREATE_FOLDER);
        add(Action::CREATE_FILE);
        separator();
        add(Action::SELECT_ALL);
        add(Action::INVERT_SELECTION);
        add(Action::SELECT_PATTERN);
        separator();
        add(Action::REFRESH);
        if (request.folder && !request.folder->isTrash())
          add(m_pins.contains(*request.folder) ? Action::UNPIN_CURRENT : Action::PIN_CURRENT);
        add(Action::RELOAD_PINS);
        separator();
        add(Action::COPY_PATH);
        add(Action::COPY_URI);
        add(Action::TERMINAL);
      } else {
        if (request.targets.empty())
          return;
        add(Action::OPEN);
        if (request.targets.size() == 1) {
          add(Action::CONTAINING_FOLDER);
          if (m_browser.model().entry(request.targets.front().id)->trash.original)
            add(Action::ORIGINAL_LOCATION);
        }
        const bool folders = std::ranges::all_of(request.targets, [this](const auto& target) {
          const auto* entry = m_browser.model().entry(target.id);
          return entry->type == G_FILE_TYPE_DIRECTORY && !entry->location.isTrash();
        });
        if (folders) {
          separator();
          if (std::ranges::any_of(request.targets, [this](const auto& target) {
                return !m_pins.contains(target.location);
              }))
            add(Action::PIN);
          if (std::ranges::any_of(request.targets, [this](const auto& target) {
                return m_pins.contains(target.location);
              }))
            add(Action::UNPIN);
        }
        separator();
        const bool trash = request.targets.front().location.isTrash();
        if (request.targets.size() == 1 && !trash)
          add(Action::RENAME);
        if (!trash)
          add(Action::TRASH);
        separator();
        add(Action::COPY_TARGET_PATH);
        add(Action::COPY_TARGET_URI);
        if (folders && request.targets.size() == 1)
          add(Action::TARGET_TERMINAL);
        separator();
        add(Action::PROPERTIES);
      }
      showContextMenu(
          std::move(entries), [this, request](int action) { dispatchAction(static_cast<Action>(action), request); },
          {x, y, 1, 1}
      );
    }

    void openPlaceMenu(const files::Bookmark& bookmark, float x, float y) {
      std::vector<ContextMenuControlEntry> entries{{.id = 1, .label = "Open"}};
      {
        const auto& pins = m_pins.bookmarks();
        const auto found = std::ranges::find(pins, bookmark.location, &files::Bookmark::location);
        const auto index = static_cast<std::size_t>(found - pins.begin());
        entries.push_back({.label = "", .separator = true});
        if (!bookmark.location.isTrash()) {
          entries.push_back({.id = 2, .label = "Unpin", .enabled = m_pins.editable()});
          entries.push_back({.id = 6, .label = "Rename place…", .enabled = m_pins.editable()});
        }
        entries.push_back({.id = 3, .label = "Move up", .enabled = m_pins.editable() && index > 0});
        entries.push_back({.id = 4, .label = "Move down", .enabled = m_pins.editable() && index + 1 < pins.size()});
      }
      entries.push_back({.label = "", .separator = true});
      if (bookmark.location == files::Location::trash()) {
        entries.push_back(
            {.id = 7,
             .label = "Empty Trash…",
             .enabled = actionState(Action::EMPTY_TRASH, actionRequest(false)).enabled,
             .destructive = true}
        );
        entries.push_back({.label = "", .separator = true});
      }
      entries.push_back({.id = 5, .label = "Properties"});
      showContextMenu(
          std::move(entries),
          [this, bookmark](int action) {
            if (!m_pins.contains(bookmark.location)) {
              actionError("This pin is no longer available.");
              return;
            }
            if (action == 1) {
              m_browser.open(bookmark.location);
            } else if (action == 2) {
              m_pins.remove(bookmark.location);
            } else if (action == 7 && bookmark.location == files::Location::trash()) {
              dispatchAction(Action::EMPTY_TRASH);
            } else if (action == 6) {
              openPlaceRename(bookmark);
            } else if (action == 3 || action == 4) {
              const auto& pins = m_pins.bookmarks();
              const auto found = std::ranges::find(pins, bookmark.location, &files::Bookmark::location);
              const auto index = static_cast<std::size_t>(found - pins.begin());
              m_pins.move(
                  bookmark.location, action == 3 ? (index > 0 ? index - 1 : 0) : std::min(index + 1, pins.size() - 1)
              );
            } else if (action == 5) {
              showProperties(
                  {{.location = bookmark.location,
                    .name = bookmark.label,
                    .displayName = bookmark.label,
                    .type = G_FILE_TYPE_DIRECTORY,
                    .size = std::nullopt,
                    .modifiedSeconds = std::nullopt,
                    .etag = std::nullopt,
                    .parentDisplayName = {}}}
              );
            }
          },
          {x, y, 1, 1}
      );
    }

    bool canEmptyTrash() const {
      return !m_transfers.busy()
          && !m_trash.busy()
          && !m_trash.canTrash()
          && !m_emptyTrash.pending()
          && !m_renamer.busy()
          && !m_creator.busy();
    }
    void openEmptyTrash() {
      if (!canEmptyTrash()) {
        actionError("Another file operation is still finishing.");
        return;
      }
      cancelDrag();
      const float width = std::min(420.0F, static_cast<float>(m_surface.width()) - 32.0F);
      Button* cancel = nullptr;
      Label* message = nullptr;
      Label* size = nullptr;
      Button* confirm = nullptr;
      auto root = ui::column(
          {.align = FlexAlign::Stretch,
           .gap = Style::spaceMd,
           .padding = Style::spaceMd,
           .fill = colorSpecFromRole(ColorRole::SurfaceContainer),
           .radius = Style::radiusLg,
           .border = colorSpecFromRole(ColorRole::OutlineVariant)},
          ui::label({.text = "Empty Trash?", .fontWeight = FontWeight::SemiBold}),
          ui::label(
              {.out = &message,
               .text = "Reading Trash…",
               .maxWidth = width - 24.0F,
               .maxLines = 4,
               .configure = [](Label& label) { label.setUseMarkup(false); }}
          ),
          ui::label(
              {.out = &size,
               .text = "Size: calculating…",
               .fontSize = Style::fontSizeCaption,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxWidth = width - 24.0F,
               .maxLines = 2}
          ),
          ui::row(
              {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
              ui::button(
                  {.out = &cancel,
                   .text = "Cancel",
                   .variant = ButtonVariant::Default,
                   .onClick = [this] { dismissMenu(); }}
              ),
              ui::button(
                  {.out = &confirm,
                   .text = "Empty Trash",
                   .enabled = false,
                   .variant = ButtonVariant::Destructive,
                   .onClick = [this] { confirmEmptyTrash(); }}
              )
          )
      );
      UiPhaseScope phase(UiPhase::Layout);
      LayoutConstraints constraints;
      constraints.setExactWidth(width);
      const float height = root->measure(m_surface.renderTarget().renderer(), constraints).height;
      showPopup(
          std::move(root), nullptr, {(m_surface.width() - width) * 0.5F, (m_surface.height() - height) * 0.5F, 1, 1},
          width, height
      );
      if (m_menuSurface != nullptr) {
        m_emptyTrashDialog = true;
        m_emptyTrashMessage = message;
        m_emptyTrashSize = size;
        m_emptyTrashConfirm = confirm;
        m_menuDispatcher.setFocus(cancel->inputArea());
        if (!m_emptyTrash.prepare())
          message->setText("Another Empty Trash operation is still finishing.");
      }
    }
    void confirmEmptyTrash() {
      if (!m_emptyTrashDialog || !m_emptyTrash.empty())
        return;
      m_emptyTrashConfirm->setEnabled(false);
      m_emptyTrashDialog = false;
      dismissMenu();
    }
    void syncEmptyTrash() {
      const auto& snapshot = m_emptyTrash.snapshot();
      if (m_emptyTrashDialog && m_emptyTrashMessage != nullptr) {
        if (snapshot.status == files::EmptyTrashStatus::READY) {
          const auto count = snapshot.results.size();
          m_emptyTrashMessage->setText(
              count == 0 ? "Trash is already empty."
                         : std::format(
                               "Permanently delete {} item{} and their contents? This cannot be undone.", count,
                               count == 1 ? "" : "s"
                           )
          );
          m_emptyTrashConfirm->setEnabled(m_emptyTrash.canEmpty());
        } else if (snapshot.status == files::EmptyTrashStatus::ERROR) {
          m_emptyTrashMessage->setText("Could not read Trash: " + snapshot.error);
          m_emptyTrashConfirm->setEnabled(false);
        }
        std::string sizeText = "Size: calculating…";
        if (snapshot.size) {
          const files::GString value(g_format_size_full(*snapshot.size, G_FORMAT_SIZE_IEC_UNITS));
          sizeText = std::format("Size: {}{}", snapshot.sizeIncomplete ? "at least " : "", value.get());
        } else if (!m_emptyTrash.busy()) {
          sizeText = "Size unavailable";
        }
        m_emptyTrashSize->setText(sizeText);
        m_menuSurface->requestLayout();
      } else if (snapshot.status == files::EmptyTrashStatus::FINISHED) {
        const auto count = [&](files::EmptyTrashOutcome outcome) {
          return std::ranges::count(snapshot.results, outcome, &files::EmptyTrashResult::outcome);
        };
        const auto done = count(files::EmptyTrashOutcome::DELETED);
        std::string message = std::format("{} item{} permanently deleted", done, done == 1 ? "" : "s");
        for (const auto& [outcome, label] :
             {std::pair{files::EmptyTrashOutcome::SKIPPED, "skipped"},
              std::pair{files::EmptyTrashOutcome::FAILED, "failed"},
              std::pair{files::EmptyTrashOutcome::CANCELLED, "cancelled"},
              std::pair{files::EmptyTrashOutcome::UNCONFIRMED, "could not be confirmed"}}) {
          if (const auto amount = count(outcome); amount != 0)
            message += std::format(" · {} {}", amount, label);
        }
        const auto error = std::ranges::find_if(snapshot.results, [](const auto& item) { return !item.error.empty(); });
        if (error != snapshot.results.end())
          message += std::format(" · {}: {}", error->displayName, error->error);
        actionError(std::move(message));
        if (const auto& location = m_browser.model().snapshot().location; location && location->isTrash())
          m_browser.open(files::Location::trash());
      } else if (snapshot.status == files::EmptyTrashStatus::ERROR) {
        actionError("Could not empty Trash: " + snapshot.error);
      }
    }

    void updateOperationProgress() {
      if (m_transfers.busy() || m_trash.busy() || m_emptyTrash.busy()) {
        m_operationProgress.startRepeating(std::chrono::milliseconds(100), [this] {
          m_browserDirty = true;
          m_surface.requestUpdate();
        });
      } else {
        m_operationProgress.stop();
      }
    }
    bool canTrashItems(const std::vector<files::DirectoryEntry>& targets) const {
      return actionState(Action::TRASH, requestFor(targets)).enabled;
    }
    void trashItems(const std::vector<files::DirectoryEntry>& targets) {
      if (!canTrashItems(targets) || !targetsCurrent(targets)) {
        actionError("This selection cannot be moved to Trash right now.");
        return;
      }
      cancelDrag();
      std::vector<files::Location> sources;
      sources.reserve(targets.size());
      for (const auto& target : targets)
        sources.push_back(target.location);
      if (!m_trash.prepare(*m_browser.model().snapshot().location, std::move(sources)))
        actionError("Another Trash operation is still finishing.");
    }
    void syncTrash() {
      const auto& snapshot = m_trash.snapshot();
      if (snapshot.status == files::TrashStatus::READY) {
        (void)m_trash.trash();
      } else if (snapshot.status == files::TrashStatus::ERROR) {
        actionError("Could not move to Trash: " + snapshot.error);
      } else if (snapshot.status == files::TrashStatus::CANCELLED) {
        actionError("Move to Trash cancelled.");
      } else if (snapshot.status == files::TrashStatus::FINISHED) {
        const auto done =
            std::ranges::count(snapshot.results, files::TrashOutcome::TRASHED, &files::TrashResult::outcome);
        const auto failed =
            std::ranges::count(snapshot.results, files::TrashOutcome::FAILED, &files::TrashResult::outcome);
        const auto cancelled =
            std::ranges::count(snapshot.results, files::TrashOutcome::CANCELLED, &files::TrashResult::outcome);
        std::string message = std::format("{} item{} moved to Trash", done, done == 1 ? "" : "s");
        if (failed != 0)
          message += std::format(" · {} failed", failed);
        if (cancelled != 0)
          message += std::format(" · {} cancelled", cancelled);
        const auto error =
            std::ranges::find_if(snapshot.results, [](const auto& result) { return !result.error.empty(); });
        if (error != snapshot.results.end()) {
          const files::GString raw(g_file_get_basename(error->source.file().get()));
          const files::GString name(g_filename_display_name(raw ? raw.get() : "Item"));
          message += std::format(" · {}: {}", name.get(), error->error);
        }
        actionError(std::move(message));
        m_browser.refresh();
      }
    }

    bool canCreateItem() const { return actionState(Action::CREATE_FOLDER, actionRequest(false)).enabled; }
    void openCreateItem(files::Location parent, files::CreateKind kind) {
      if (!canCreateItem() || m_browser.model().snapshot().location != parent) {
        actionError("Open an available writable folder to create a file or folder.");
        return;
      }
      const float width = std::min(420.0F, static_cast<float>(m_surface.width()) - 32.0F);
      auto editor = std::make_unique<NameEditor>(
          width, kind == files::CreateKind::FOLDER ? "New folder" : "New file",
          kind == files::CreateKind::FOLDER ? "New Folder" : "New File", "", files::filenameProblem,
          [this](std::string name) { submitCreateItem(std::move(name)); }, [this] { dismissMenu(); }, "Create",
          "Creating…"
      );
      auto* control = editor.get();
      UiPhaseScope phase(UiPhase::Layout);
      LayoutConstraints constraints;
      constraints.setExactWidth(width);
      const float height = editor->measure(m_surface.renderTarget().renderer(), constraints).height;
      showPopup(
          std::move(editor), nullptr, {(m_surface.width() - width) * 0.5F, (m_surface.height() - height) * 0.5F, 1, 1},
          width, height
      );
      if (m_menuSurface != nullptr) {
        m_createEditor = control;
        m_createParent = std::move(parent);
        m_creationPreparing = true;
        control->setSaving(true);
        if (!m_creator.prepare(*m_createParent, kind))
          control->showError("Another creation is still finishing.");
      }
    }
    void submitCreateItem(std::string name) {
      if (m_createEditor == nullptr || !m_createParent)
        return;
      if (m_browser.model().snapshot().location != m_createParent || !m_browser.searchQuery().empty()) {
        m_createEditor->showError("The destination changed. Close this dialog and try again.");
        return;
      }
      if (m_creator.create(std::move(name))) {
        m_createEditor->setSaving(true);
      } else {
        m_createEditor->showError(
            m_creator.snapshot().error.empty() ? "The destination is unavailable. Close this dialog and try again."
                                               : m_creator.snapshot().error
        );
      }
    }
    void syncCreation() {
      if (m_creator.busy())
        return;
      const auto& snapshot = m_creator.snapshot();
      if (snapshot.status == files::CreateStatus::SUCCEEDED || snapshot.status == files::CreateStatus::PARTIAL) {
        m_actionError = snapshot.error;
        if (m_browser.model().snapshot().location == snapshot.parent) {
          m_revealTarget = snapshot.destination;
          m_browser.refresh();
        }
        if (m_createEditor != nullptr) {
          m_createEditor = nullptr;
          dismissMenu();
        }
      } else if (m_createEditor == nullptr) {
        if (snapshot.status == files::CreateStatus::ERROR)
          actionError("Could not create: " + snapshot.error);
      } else if (snapshot.status == files::CreateStatus::READY || snapshot.status == files::CreateStatus::ERROR) {
        m_createEditor->setSaving(false);
        m_createEditor->showError(snapshot.error);
        if (std::exchange(m_creationPreparing, false)) {
          m_menuDispatcher.setFocus(m_createEditor->input()->inputArea());
          m_createEditor->input()->selectAll();
        }
        m_menuSurface->requestLayout();
      }
    }

    bool canFileRename(const files::DirectoryEntry& entry) const {
      return actionState(Action::RENAME, requestFor({entry})).enabled;
    }
    void openFileRename(files::DirectoryEntry target) {
      if (!canFileRename(target) || !targetsCurrent({target})) {
        actionError("This item cannot be renamed right now.");
        return;
      }
      const float width = std::min(420.0F, static_cast<float>(m_surface.width()) - 32.0F);
      const bool validText = g_utf8_validate(target.name.data(), static_cast<gssize>(target.name.size()), nullptr);
      auto editor = std::make_unique<NameEditor>(
          width, target.type == G_FILE_TYPE_DIRECTORY && !target.symlink ? "Rename folder" : "Rename file",
          validText ? target.name : "", validText ? "" : "Enter a new UTF-8 name for this item.",
          files::filenameProblem, [this](std::string name) { submitFileRename(std::move(name)); },
          [this] { dismissMenu(); }
      );
      auto* control = editor.get();
      UiPhaseScope phase(UiPhase::Layout);
      LayoutConstraints constraints;
      constraints.setExactWidth(width);
      const float height = editor->measure(m_surface.renderTarget().renderer(), constraints).height;
      const LayoutRect anchor{(m_surface.width() - width) * 0.5F, (m_surface.height() - height) * 0.5F, 1, 1};
      showPopup(std::move(editor), nullptr, anchor, width, height);
      if (m_menuSurface != nullptr) {
        m_fileRenameEditor = control;
        m_fileRenameTarget = std::move(target);
        m_fileRenamePreparing = true;
        control->setSaving(true);
        if (!m_renamer.prepare(m_fileRenameTarget->location)) {
          control->showError("Another rename is still finishing.");
        }
      }
    }
    void submitFileRename(std::string name) {
      if (m_fileRenameEditor == nullptr || !m_fileRenameTarget)
        return;
      if (!targetsCurrent({*m_fileRenameTarget})) {
        m_fileRenameEditor->showError("The item changed. Close this editor and try again.");
        return;
      }
      if (!m_renamer.rename(std::move(name))) {
        m_fileRenameEditor->showError(
            m_renamer.snapshot().error.empty() ? "The item is unavailable. Reopen Rename." : m_renamer.snapshot().error
        );
      } else {
        m_fileRenameEditor->setSaving(true);
      }
    }
    void syncFileRename() {
      const auto& snapshot = m_renamer.snapshot();
      if (m_renamer.busy())
        return;
      if (snapshot.status == files::RenameStatus::SUCCEEDED) {
        m_actionError.clear();
        const auto& current = m_browser.model().snapshot().location;
        if (current && snapshot.source && g_file_has_prefix(snapshot.source->file().get(), current->file().get())) {
          m_revealTarget = snapshot.destination;
          m_browser.refresh();
        }
        if (m_fileRenameEditor != nullptr) {
          m_fileRenameEditor = nullptr;
          dismissMenu();
        }
      } else if (m_fileRenameEditor == nullptr) {
        if (snapshot.status == files::RenameStatus::ERROR)
          actionError("Could not rename: " + snapshot.error);
      } else if (snapshot.status == files::RenameStatus::READY || snapshot.status == files::RenameStatus::ERROR) {
        m_fileRenameEditor->setSaving(false);
        m_fileRenameEditor->showError(snapshot.error);
        if (std::exchange(m_fileRenamePreparing, false)) {
          m_menuDispatcher.setFocus(m_fileRenameEditor->input()->inputArea());
          const auto& value = m_fileRenameEditor->input()->value();
          const bool directory = m_fileRenameTarget->type == G_FILE_TYPE_DIRECTORY && !m_fileRenameTarget->symlink;
          m_fileRenameEditor->input()->selectRange(0, files::filenameStemLength(value, directory));
        }
        m_menuSurface->requestLayout();
      }
    }

    void openPlaceRename(const files::Bookmark& bookmark) {
      if (bookmark.location.isTrash())
        return;
      const auto found = std::ranges::find(m_pins.bookmarks(), bookmark.location, &files::Bookmark::location);
      if (!m_pins.editable() || found == m_pins.bookmarks().end()) {
        actionError("This place is unavailable for editing. Reload Places and try again.");
        return;
      }
      const auto current = *found; // NOLINT(performance-unnecessary-copy-initialization): own the editing snapshot.
      const float width = std::min(440.0F, static_cast<float>(m_surface.width()) - 32.0F);
      auto editor = std::make_unique<NameEditor>(
          width, "Rename place", current.label, "Changes the sidebar label only.",
          [](const std::string& label) -> std::optional<std::string> {
            if (const auto error = files::bookmarkLabelProblem(label))
              return error;
            if (label.find_first_not_of(' ') == std::string::npos)
              return "Enter a label.";
            return std::nullopt;
          },
          [this](std::string label) { submitPlaceRename(std::move(label)); }, [this] { dismissMenu(); }
      );
      auto* control = editor.get();
      UiPhaseScope phase(UiPhase::Layout);
      LayoutConstraints constraints;
      constraints.setExactWidth(width);
      const float height = std::min(
          editor->measure(m_surface.renderTarget().renderer(), constraints).height,
          static_cast<float>(m_surface.height()) - 64.0F
      );
      showPopup(
          std::move(editor), nullptr, {(m_surface.width() - width) * 0.5F, (m_surface.height() - height) * 0.5F, 1, 1},
          width, height
      );
      if (m_menuSurface != nullptr) {
        m_placeEditor = control;
        m_placeRenameTarget = current;
        m_menuDispatcher.setFocus(control->input()->inputArea());
        control->input()->selectAll();
      }
    }

    void submitPlaceRename(std::string label) {
      if (m_placeEditor == nullptr || !m_placeRenameTarget) {
        return;
      }
      m_pendingPlaceLabel = label;
      m_placeEditor->setSaving(true);
      if (!m_pins.rename(*m_placeRenameTarget, std::move(label))) {
        m_pendingPlaceLabel.reset();
        m_placeEditor->setSaving(false);
        m_placeEditor->showError(
            m_pins.busy() ? "Places are being saved. Try again shortly."
                          : "This place changed. Close this dialog and try again."
        );
        return;
      }
      syncPlaceRename();
    }

    void syncPlaceRename() {
      if (m_placeEditor == nullptr || !m_pendingPlaceLabel || m_pins.busy()) {
        return;
      }
      const auto found =
          std::ranges::find(m_pins.bookmarks(), m_placeRenameTarget->location, &files::Bookmark::location);
      if (found != m_pins.bookmarks().end() && found->label == *m_pendingPlaceLabel) {
        dismissMenu();
      } else {
        m_placeEditor->setSaving(false);
        m_placeEditor->showError(
            m_pins.error().empty() ? "This place changed. Close this dialog and try again."
                                   : "Could not save the label. Close this dialog and try again."
        );
      }
      m_pendingPlaceLabel.reset();
    }

    void showContaining(const files::Location& location) {
      const files::GioPtr<GFile> parent(g_file_get_parent(location.file().get()));
      if (!parent) {
        return;
      }
      const files::GString uri(g_file_get_uri(parent.get()));
      m_browser.open(files::Location::fromUri(uri.get()));
      m_revealTarget = location;
      m_dispatcher.setFocus(m_list->focusArea());
    }

    void applySettings(const files::BrowserSettings& settings, bool force) {
      const auto before = m_savedSettings;
      if (!force && before == settings)
        return;
      captureViewAnchor();
      m_dispatcher.cancelPointerCapture();
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      if (m_menu && !m_settingsView)
        closeMenu();
      if (force || before.viewMode != settings.viewMode) {
        m_viewMode = settings.viewMode;
        m_browser.setDefaultViewMode(settings.viewMode);
      }
      if (force || before.gridIconSize != settings.gridIconSize) {
        m_gridZoom = static_cast<std::size_t>(
            std::ranges::find(files::kGridIconSizes, settings.gridIconSize) - files::kGridIconSizes.begin()
        );
        m_zoomScroll = 0;
        m_browser.setDefaultIconSize(settings.gridIconSize);
      }
      if (force || before.sort != settings.sort)
        m_browser.model().setSort(settings.sort);
      if ((force || before.showHidden != settings.showHidden)
          && m_browser.model().filter().showHidden != settings.showHidden)
        m_browser.toggleHidden();
      if (force || before.activationMode != settings.activationMode || before.typingMode != settings.typingMode) {
        clearTypeahead();
        m_activationMode = settings.activationMode;
        m_typingMode = settings.typingMode;
        for (auto* adapter : {&m_listAdapter, &m_gridAdapter})
          adapter->setActivationMode(m_activationMode);
      }
      if (force || before.showSidebar != settings.showSidebar)
        setSidebarVisible(settings.showSidebar, false);
      if (force || before.sidebarWidth != settings.sidebarWidth)
        m_sidebarWidth = settings.sidebarWidth;
      if (force || before.columns != settings.columns)
        m_columnVisibility = settings.columns;
      if (force || before.columnOrder != settings.columnOrder)
        m_columnOrder = settings.columnOrder;
      if (force || before.columnWidths != settings.columnWidths)
        m_columnWidths = settings.columnWidths;
      if (force || before.thumbnails != settings.thumbnails)
        m_thumbnails.setOptions(settings.thumbnails);
      m_savedSettings = settings;
      m_browserDirty = true;
      m_surface.requestLayout();
    }

    void showCenteredDialog(std::unique_ptr<Flex> root) {
      const float width = std::min(588.0F, static_cast<float>(m_surface.width()) - 32.0F);
      UiPhaseScope phase(UiPhase::Layout);
      LayoutConstraints constraints;
      constraints.setExactWidth(width);
      const float preferredHeight = root->measure(m_surface.renderTarget().renderer(), constraints).height;
      const float height = std::min(preferredHeight, static_cast<float>(m_surface.height()) - 64.0F);
      showPopup(
          std::move(root), nullptr, {(m_surface.width() - width) * 0.5F, (m_surface.height() - height) * 0.5F, 1, 1},
          width, height
      );
      if (!m_menuSurface)
        return;
      m_dialogHeight = preferredHeight;
      m_menuDispatcher.setFocusChangeCallback([this](InputArea*, InputArea* focused) {
        if (m_aboutScroll)
          m_aboutScroll->requestScrollIntoView(focused);
        if (m_menuSurface)
          m_menuSurface->requestLayout();
      });
      (void)m_menuDispatcher.cycleTabFocus(false);
      m_menuSurface->requestLayout();
    }

    void deferCloseSettings() {
      const std::weak_ptr<int> lifetime = m_lifetime;
      DeferredCall::callLater([this, lifetime] {
        if (!lifetime.expired())
          closeSettings();
      });
    }
    void closeSettings() {
      if (!m_settingsView)
        return;
      closeMenu();
      m_settingsLifetime.reset();
      m_dispatcher.cancelPointerCapture();
      m_dispatcher.setFocus(m_list->focusArea());
      auto* settings = std::exchange(m_settingsView, nullptr);
      auto removed = m_root->removeChild(settings);
      m_surface.requestLayout();
    }
    void openSettings() {
      if (m_settingsView) {
        deferCloseSettings();
        return;
      }
      closeMenu();
      cancelDrag();
      clearCompletions();
      if (m_editingLocation)
        endLocationEdit(false);
      m_settingsLifetime = std::make_shared<int>(0);
      auto root = std::make_unique<BrowserSettingsView>(
          [this](files::OverrideEdit edit) {
            m_preferences.submit({edit});
            m_surface.requestLayout();
          },
          [this] { m_preferences.resetAll(); }, [this] { deferCloseSettings(); }, [this] { m_thumbnails.clearCache(); }
      );
      root->setParticipatesInLayout(false);
      root->setZIndex(1000);
      root->setPopupContext(this);
      m_settingsView = root.get();
      const auto& inherited = m_preferences.inheritedSettings();
      root->sync(
          m_savedSettings, inherited ? inherited->browser : files::BrowserSettings{},
          m_preferences.busy() || m_thumbnails.clearing(),
          m_preferenceError.empty() ? m_thumbnails.error() : m_preferenceError, m_thumbnails.clearing()
      );
      m_root->addChild(std::move(root));
      m_dispatcher.cancelPointerCapture();
      m_dispatcher.setFocus(m_dispatcher.firstTabFocusUnder(m_settingsView));
      m_surface.requestLayout();
    }
    void openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) override {
      if (!m_settingsView)
        return;
      std::vector<ContextMenuControlEntry> entries;
      entries.reserve(request.options.size());
      for (std::size_t index = 0; index < request.options.size(); ++index)
        entries.push_back(
            {.id = static_cast<int>(index),
             .label = request.options[index],
             .checkmark = true,
             .toggleState = index == request.selectedIndex ? 1 : 0}
        );
      const std::weak_ptr<int> lifetime = m_settingsLifetime;
      showContextMenu(
          std::move(entries),
          [lifetime, select = callbacks.onSelect](int index) {
            if (!lifetime.expired() && select)
              select(static_cast<std::size_t>(index));
          },
          {static_cast<float>(request.anchorX), static_cast<float>(request.anchorY),
           static_cast<float>(request.anchorWidth), static_cast<float>(request.anchorHeight)},
          true, request.menuWidth
      );
      if (m_menu) {
        m_menu->setHighlightedIndex(request.selectedIndex);
        revealMenuHighlight();
      }
      m_selectDismiss = [lifetime, dismiss = callbacks.onDismiss] {
        if (!lifetime.expired() && dismiss)
          dismiss();
      };
    }
    void closeSelectDropdown() override { dismissMenu(); }
    bool isSelectDropdownOpen() const override { return m_menuSurface && static_cast<bool>(m_selectDismiss); }

    void openAbout() {
      ScrollView* scroll = nullptr;
      Label* message = nullptr;
      constexpr float width = 536.0F;
      auto body = ui::scrollView(
          {.out = &scroll,
           .viewportPaddingH = 0.0F,
           .viewportPaddingV = 0.0F,
           .fillWidth = true,
           .flexGrow = 1.0F,
           .configure = [](ScrollView& view) { view.setReserveScrollbarGutter(true); }}
      );
      body->content()->setAlign(FlexAlign::Stretch);
      body->content()->setGap(Style::spaceMd);
      const auto text = [&](const std::string& value, bool caption = false) {
        body->content()->addChild(
            ui::label(
                {.text = value,
                 .fontSize = caption ? Style::fontSizeCaption : Style::fontSizeBody,
                 .maxWidth = width,
                 .maxLines = 1000,
                 .configure = [](Label& label) { label.setUseMarkup(false); }}
            )
        );
      };
      text(
          std::format(
              "Version {} · {}\nRevision {}\nBuilt with {}", NOCTALIA_FILES_VERSION, NOCTALIA_FILES_BUILD_TYPE,
              NOCTALIA_FILES_BUILD_REVISION, NOCTALIA_FILES_COMPILER
          )
      );
      text("A standalone file manager from Noctalia.");
      const auto link = [this](const char* address) { m_launcher.open(files::Location::fromUri(address)); };
      body->content()->addChild(
          ui::row(
              {.gap = Style::spaceSm},
              ui::button(
                  {.text = "Project website",
                   .variant = ButtonVariant::Default,
                   .onClick = [link] { link("https://noctalia.dev"); }}
              ),
              ui::button({.text = "Source code", .variant = ButtonVariant::Default, .onClick = [link] {
                            link("https://github.com/noctalia-dev/noctalia-files");
                          }})
          )
      );
      body->content()->addChild(
          ui::label(
              {.out = &message,
               .color = colorSpecFromRole(ColorRole::Error),
               .maxWidth = width,
               .maxLines = 3,
               .visible = false,
               .participatesInLayout = false,
               .configure = [](Label& label) { label.setUseMarkup(false); }}
          )
      );
      auto root = ui::column(
          {.align = FlexAlign::Stretch,
           .gap = Style::spaceMd,
           .padding = Style::spaceMd,
           .fill = colorSpecFromRole(ColorRole::SurfaceContainer),
           .radius = Style::radiusLg,
           .border = colorSpecFromRole(ColorRole::OutlineVariant)},
          ui::row(
              {.align = FlexAlign::Center},
              ui::label({.text = "About Noctalia Files", .fontWeight = FontWeight::SemiBold, .flexGrow = 1.0F}),
              ui::button(
                  {.glyph = "x",
                   .controlHeight = Style::controlHeightSm,
                   .variant = ButtonVariant::Default,
                   .minWidth = Style::controlHeightSm,
                   .maxWidth = Style::controlHeightSm,
                   .onClick = [this] { dismissMenu(); }}
              )
          ),
          std::move(body)
      );
      showCenteredDialog(std::move(root));
      if (m_menuSurface) {
        m_aboutScroll = scroll;
        m_aboutMessage = message;
      }
    }

    void showProperties(const std::vector<files::DirectoryEntry>& targets) {
      const float width = std::min(440.0F, static_cast<float>(m_surface.width()) - 32.0F);
      auto body =
          ui::scrollView({.viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F, .fillWidth = true, .flexGrow = 1.0F});
      body->content()->setAlign(FlexAlign::Stretch);
      body->content()->setGap(Style::spaceMd);
      const auto literal = [](Label& label) { label.setUseMarkup(false); };
      const auto field = [&](const std::string& name, const std::string& value) {
        body->content()->addChild(
            ui::row(
                {.align = FlexAlign::Start, .gap = Style::spaceMd},
                ui::label(
                    {.text = name,
                     .fontSize = Style::fontSizeCaption,
                     .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                     .minWidth = 76.0F,
                     .maxWidth = 76.0F}
                ),
                ui::label(
                    {.text = value, .maxWidth = width - 132.0F, .maxLines = 3, .flexGrow = 1.0F, .configure = literal}
                )
            )
        );
      };
      if (targets.size() == 1) {
        const auto& entry = targets.front();
        auto icon = std::make_unique<files::FileIcon>(m_icons, 48.0F);
        icon->bind(entry.icon, colorSpecFromRole(ColorRole::OnSurface));
        body->content()->addChild(
            ui::row(
                {.align = FlexAlign::Center, .gap = Style::spaceMd}, std::move(icon),
                ui::column(
                    {.align = FlexAlign::Start, .gap = Style::spaceXs, .flexGrow = 1.0F},
                    ui::label(
                        {.text = entry.displayName,
                         .fontWeight = FontWeight::SemiBold,
                         .maxWidth = width - 104.0F,
                         .maxLines = 2,
                         .configure = literal}
                    ),
                    ui::label(
                        {.text = entry.symlink                     ? "Symbolic link"
                             : entry.type == G_FILE_TYPE_DIRECTORY ? "Folder"
                                                                   : "File",
                         .fontSize = Style::fontSizeCaption,
                         .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
                    )
                )
            )
        );
        body->content()->addChild(ui::separator({}));
        field("Location", files::locationEditText(entry.location));
        if (entry.location.isTrash()) {
          field("Original", entry.trash.original ? files::locationEditText(*entry.trash.original) : "Unknown");
          field("Deleted", entry.trash.deletionDate.value_or("Unknown"));
        }
        if (entry.type != G_FILE_TYPE_DIRECTORY) {
          const files::GString size(entry.size ? g_format_size_full(*entry.size, G_FORMAT_SIZE_IEC_UNITS) : nullptr);
          field("Size", size ? size.get() : "Unknown");
        }
        if (entry.modifiedSeconds && *entry.modifiedSeconds <= static_cast<std::uint64_t>(G_MAXINT64)) {
          const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> date(
              g_date_time_new_from_unix_local(static_cast<gint64>(*entry.modifiedSeconds)), &g_date_time_unref
          );
          if (date) {
            const files::GString text(g_date_time_format(date.get(), "%Y-%m-%d %H:%M:%S"));
            field("Modified", text.get());
          }
        }
      } else {
        field("Selection", std::format("{} items", targets.size()));
      }
      auto root = ui::column(
          {.align = FlexAlign::Stretch,
           .gap = Style::spaceMd,
           .padding = Style::spaceMd,
           .fill = colorSpecFromRole(ColorRole::SurfaceContainer),
           .radius = Style::radiusLg,
           .border = colorSpecFromRole(ColorRole::OutlineVariant)},
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm},
              ui::label({.text = "Properties", .fontWeight = FontWeight::SemiBold, .flexGrow = 1.0F}),
              ui::button(
                  {.glyph = "x",
                   .controlHeight = Style::controlHeightSm,
                   .variant = ButtonVariant::Default,
                   .tooltip = "Close",
                   .minWidth = Style::controlHeightSm,
                   .maxWidth = Style::controlHeightSm,
                   .onClick = [this] { dismissMenu(); }}
              )
          ),
          std::move(body)
      );
      UiPhaseScope phase(UiPhase::Layout);
      auto& renderer = m_surface.renderTarget().renderer();
      LayoutConstraints constraints;
      constraints.setExactWidth(width);
      const float height =
          std::min(root->measure(renderer, constraints).height, static_cast<float>(m_surface.height()) - 64.0F);
      showPopup(
          std::move(root), nullptr, {(m_surface.width() - width) * 0.5F, (m_surface.height() - height) * 0.5F, 1, 1},
          width, height
      );
    }

    void syncTransferConflict() {
      const auto conflict = m_transfers.conflict();
      if (m_activeTransferConflict && !conflict) {
        dismissMenu();
        return;
      }
      if (!conflict || m_menuSurface != nullptr) {
        return;
      }
      const float width = std::min(520.0F, static_cast<float>(m_surface.width()) - 32.0F);
      auto facts = [&](const std::string& title, bool folder, std::optional<std::uint64_t> bytes,
                       std::optional<std::uint64_t> modified) {
        std::string description = folder ? "Folder" : "File";
        if (!folder && bytes) {
          const files::GString size(g_format_size_full(*bytes, G_FORMAT_SIZE_IEC_UNITS));
          description += " · " + std::string(size.get());
        }
        std::string date = "Modification time unknown";
        if (modified && *modified <= static_cast<std::uint64_t>(G_MAXINT64)) {
          const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> value(
              g_date_time_new_from_unix_local(static_cast<gint64>(*modified)), &g_date_time_unref
          );
          if (value) {
            const files::GString text(g_date_time_format(value.get(), "%Y-%m-%d %H:%M"));
            date = text.get();
          }
        }
        return ui::column(
            {.align = FlexAlign::Stretch, .gap = Style::spaceXs, .flexGrow = 1.0F},
            ui::label(
                {.text = title,
                 .fontSize = Style::fontSizeCaption,
                 .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
            ),
            ui::label({.text = description, .maxWidth = (width - 48.0F) * 0.5F, .maxLines = 1}),
            ui::label(
                {.text = date,
                 .fontSize = Style::fontSizeCaption,
                 .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                 .maxWidth = (width - 48.0F) * 0.5F,
                 .maxLines = 1}
            )
        );
      };
      const auto choose = [this, id = conflict->id](files::TransferConflictChoice choice) {
        if (m_transfers.resolve(id, choice)) {
          m_activeTransferConflict.reset();
          dismissMenu();
        }
      };
      Button* keepBoth = nullptr;
      auto root = ui::column(
          {.align = FlexAlign::Stretch,
           .gap = Style::spaceMd,
           .padding = Style::spaceMd,
           .fill = colorSpecFromRole(ColorRole::SurfaceContainer),
           .radius = Style::radiusLg,
           .border = colorSpecFromRole(ColorRole::OutlineVariant)},
          ui::label({.text = "Item already exists", .fontWeight = FontWeight::SemiBold}),
          ui::label(
              {.text = conflict->name,
               .maxWidth = width - 24.0F,
               .maxLines = 2,
               .configure = [](Label& name) { name.setUseMarkup(false); }}
          ),
          ui::separator({}),
          ui::row(
              {.align = FlexAlign::Start, .gap = Style::spaceMd},
              facts("Incoming", conflict->sourceDirectory, conflict->sourceSize, conflict->sourceModified),
              facts(
                  "Existing", conflict->destinationDirectory, conflict->destinationSize, conflict->destinationModified
              )
          ),
          ui::label(
              {.text = "Replace removes the existing folder and its contents.",
               .fontSize = Style::fontSizeCaption,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxWidth = width - 24.0F,
               .maxLines = 2,
               .visible = conflict->destinationDirectory,
               .participatesInLayout = conflict->destinationDirectory}
          ),
          ui::row(
              {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
              ui::button(
                  {.text = "Cancel",
                   .variant = ButtonVariant::Ghost,
                   .onClick = [choose] { choose(files::TransferConflictChoice::CANCEL); }}
              ),
              ui::button(
                  {.text = "Skip",
                   .variant = ButtonVariant::Ghost,
                   .onClick = [choose] { choose(files::TransferConflictChoice::SKIP); }}
              ),
              ui::button(
                  {.out = &keepBoth,
                   .text = "Keep both",
                   .variant = ButtonVariant::Primary,
                   .onClick = [choose] { choose(files::TransferConflictChoice::KEEP_BOTH); }}
              ),
              ui::button({.text = "Replace", .variant = ButtonVariant::Destructive, .onClick = [choose] {
                            choose(files::TransferConflictChoice::REPLACE);
                          }})
          )
      );
      UiPhaseScope phase(UiPhase::Layout);
      LayoutConstraints constraints;
      constraints.setExactWidth(width);
      const float height = std::min(
          root->measure(m_surface.renderTarget().renderer(), constraints).height,
          static_cast<float>(m_surface.height()) - 64.0F
      );
      showPopup(
          std::move(root), nullptr, {(m_surface.width() - width) * 0.5F, (m_surface.height() - height) * 0.5F, 1, 1},
          width, height
      );
      if (m_menuSurface != nullptr) {
        m_activeTransferConflict = conflict->id;
        m_menuDispatcher.setFocus(keepBoth->inputArea());
      } else {
        (void)m_transfers.resolve(conflict->id, files::TransferConflictChoice::CANCEL);
      }
    }

    bool dragCurrent() const {
      if (!m_drag) {
        return false;
      }
      if (m_drag->sourcePin) {
        return m_pins.editable() && m_pins.contains(*m_drag->sourcePin);
      }
      return !m_transfers.busy()
          && !m_trash.busy()
          && !m_trash.canTrash()
          && !m_emptyTrash.pending()
          && !m_renamer.busy()
          && !m_creator.busy()
          && std::ranges::all_of(m_drag->entries, [this](const auto& target) {
               const auto* entry = m_browser.model().entry(target.id);
               return entry != nullptr
                   && entry->location == target.location
                   && std::ranges::contains(m_browser.model().rows(), target.id);
             });
    }

    bool validDropFolder(const files::Location& destination) const {
      if (!m_drag || m_drag->sourcePin || !destination.nativePath()) {
        return false;
      }
      bool anyDifferentParent = false;
      for (const auto& source : m_drag->entries) {
        if (!source.location.nativePath() || source.location == destination) {
          return false;
        }
        const auto* entry = m_browser.model().entry(source.id);
        if (entry == nullptr) {
          return false;
        }
        if (entry->type == G_FILE_TYPE_DIRECTORY
            && g_file_has_prefix(destination.file().get(), source.location.file().get())) {
          return false;
        }
        const files::GioPtr<GFile> parent(g_file_get_parent(source.location.file().get()));
        anyDifferentParent = anyDifferentParent || !parent || !g_file_equal(parent.get(), destination.file().get());
      }
      return anyDifferentParent || (m_connection.input().keyboardModifiers() & KeyMod::Ctrl) != 0;
    }

    std::optional<DropTarget> dropTarget() const {
      if (!m_drag || m_drag->sourcePin) {
        return std::nullopt;
      }
      if (const auto place = m_showSidebar ? m_places->folderAt(m_pointerX, m_pointerY) : std::nullopt) {
        if (validDropFolder(place->location)) {
          if (const auto rect = m_places->folderRect(place->location)) {
            return DropTarget{place->location, *rect};
          }
        }
        return std::nullopt;
      }
      if (!m_editingLocation && m_breadcrumbScroll->containsScenePoint(m_pointerX, m_pointerY)) {
        for (const auto& [location, button] : m_dropBreadcrumbs) {
          if (button->containsScenePoint(m_pointerX, m_pointerY) && validDropFolder(location)) {
            return DropTarget{location, sceneRect(*button)};
          }
        }
      }
      if (m_up->containsScenePoint(m_pointerX, m_pointerY) && m_browser.model().snapshot().location) {
        const files::GioPtr<GFile> parent(g_file_get_parent(m_browser.model().snapshot().location->file().get()));
        if (parent) {
          const files::GString uri(g_file_get_uri(parent.get()));
          const auto location = files::Location::fromUri(uri.get());
          if (validDropFolder(location)) {
            return DropTarget{location, sceneRect(*m_up)};
          }
        }
      }
      if (!m_filePane->containsScenePoint(m_pointerX, m_pointerY)) {
        return std::nullopt;
      }
      if (m_list->visible()) {
        if (const auto index = m_list->itemAtScene(m_pointerX, m_pointerY)) {
          const auto& rows = m_browser.model().rows();
          if (*index < rows.size()) {
            const auto* entry = m_browser.model().entry(rows[*index]);
            if (entry != nullptr
                && entry->type == G_FILE_TYPE_DIRECTORY
                && !entry->symlink
                && validDropFolder(entry->location)) {
              if (const auto rect = m_list->itemRect(*index)) {
                return DropTarget{entry->location, *rect, entry->id};
              }
            }
          }
          return std::nullopt;
        }
      }
      const auto& location = m_browser.model().snapshot().location;
      if (location && validDropFolder(*location)) {
        return DropTarget{*location, sceneRect(*m_filePane)};
      }
      return std::nullopt;
    }

    std::optional<std::size_t> pinDropSlot() const {
      if (!m_drag || !m_pins.editable() || !m_showSidebar) {
        return std::nullopt;
      }
      if (!m_drag->sourcePin && (!m_drag->foldersOnly || m_places->folderAt(m_pointerX, m_pointerY))) {
        return std::nullopt;
      }
      return m_places->dropSlot(m_pointerX, m_pointerY);
    }

    void startFileDrag(const std::vector<files::EntryId>& ids) {
      if (ids.empty()
          || m_transfers.busy()
          || m_trash.busy()
          || m_trash.canTrash()
          || m_emptyTrash.pending()
          || m_renamer.busy()
          || m_creator.busy()) {
        return;
      }
      Drag drag;
      drag.foldersOnly = true;
      for (const auto id : ids) {
        const auto* entry = m_browser.model().entry(id);
        if (entry == nullptr
            || entry->location.isTrash()
            || (entry->type != G_FILE_TYPE_DIRECTORY && entry->type != G_FILE_TYPE_REGULAR && !entry->symlink)) {
          return;
        }
        drag.foldersOnly = drag.foldersOnly && entry->type == G_FILE_TYPE_DIRECTORY && !entry->symlink;
        drag.entries.push_back({id, entry->location});
        drag.bookmarks.push_back({entry->displayName, entry->location});
      }
      m_drag = std::move(drag);
      updateDrag();
    }
    void startPlaceDrag(const files::Bookmark& bookmark) {
      if (!m_pins.editable()) {
        return;
      }
      m_drag = Drag{.entries = {}, .bookmarks = {bookmark}, .sourcePin = bookmark.location, .foldersOnly = true};
      updateDrag();
    }
    void cancelDrag() {
      if (!m_drag) {
        return;
      }
      m_drag.reset();
      m_lastDropTarget.reset();
      m_dragScroll.stop();
      m_dragScrollDelta = 0;
      m_places->cancelDragCandidate();
      m_dispatcher.cancelPointerCapture();
      if (m_dragIcon != nullptr) {
        m_dragIcon->setVisible(false);
        m_dragCount->setVisible(false);
        m_placeDropLine->setVisible(false);
        m_folderDropHighlight->setVisible(false);
      }
      m_connection.input().setCursorShape(m_connection.input().lastSerial(), WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
      m_surface.requestLayout();
    }
    float dragScrollSpeed(bool& places) const {
      places = m_showSidebar && m_places->containsScenePoint(m_pointerX, m_pointerY);
      if (places) {
        return m_places->dragScrollSpeed(m_pointerX, m_pointerY);
      }
      if (!m_drag || m_drag->sourcePin || !m_list->visible()) {
        return 0;
      }
      auto& scroll = m_list->scrollView();
      if (!scroll.containsScenePoint(m_pointerX, m_pointerY)) {
        return 0;
      }
      float x = 0, y = 0;
      Node::absolutePosition(&scroll, x, y);
      if (m_pointerY < y + 32 && scroll.scrollOffset() > 0) {
        return -6;
      }
      if (m_pointerY > y + scroll.height() - 32 && scroll.scrollOffset() < scroll.maxScrollOffset()) {
        return 6;
      }
      return 0;
    }
    void updateDrag() {
      if (!dragCurrent()) {
        cancelDrag();
        return;
      }
      m_lastDropTarget = dropTarget();
      bool places = false;
      const auto speed = dragScrollSpeed(places);
      if (places != m_dragScrollInPlaces) {
        m_dragScrollDelta = 0;
        m_dragScrollInPlaces = places;
      }
      if (speed == 0) {
        m_dragScroll.stop();
        m_dragScrollDelta = 0;
      } else if (!m_dragScroll.active()) {
        m_dragScroll.startRepeating(std::chrono::milliseconds(16), [this] {
          if (!dragCurrent()) {
            cancelDrag();
            return;
          }
          bool places = false;
          const auto speed = dragScrollSpeed(places);
          if (places != m_dragScrollInPlaces) {
            m_dragScrollDelta = 0;
            m_dragScrollInPlaces = places;
          }
          if (speed == 0) {
            m_dragScroll.stop();
            return;
          }
          m_dragScrollDelta = std::clamp(m_dragScrollDelta + speed, -18.0F, 18.0F);
          m_surface.requestLayout();
        });
      }
      m_surface.requestLayout();
    }
    void layoutDrag(Renderer& renderer) {
      if (m_drag && !dragCurrent()) {
        cancelDrag();
      }
      const bool active = m_drag.has_value();
      m_dragIcon->setVisible(active);
      m_dragCount->setVisible(active && m_drag->bookmarks.size() > 1);
      m_placeDropLine->setVisible(false);
      m_folderDropHighlight->setVisible(false);
      if (!active) {
        return;
      }
      files::FileIconSource icon{.names = {"folder"}, .filePath = {}, .desktopFile = {}};
      if (m_drag->sourcePin == files::Location::trash())
        icon.names = {"user-trash"};
      if (!m_drag->entries.empty()) {
        if (const auto* entry = m_browser.model().entry(m_drag->entries.front().id)) {
          icon = entry->icon;
        }
      }
      m_dragIcon->bind(icon, colorSpecFromRole(ColorRole::OnSurface));
      const float x = std::clamp(m_pointerX + 12, 0.0F, static_cast<float>(m_surface.width()) - 48.0F);
      const float y = std::clamp(m_pointerY + 12, 0.0F, static_cast<float>(m_surface.height()) - 48.0F);
      m_dragIcon->arrange(renderer, {x, y, 40, 40});
      m_dragCountLabel->setText(std::to_string(m_drag->bookmarks.size()));
      m_dragCount->arrange(renderer, {x + 24, y + 28, 30, 20});
      m_lastDropTarget = dropTarget();
      const auto slot = pinDropSlot();
      if (m_lastDropTarget) {
        m_folderDropHighlight->setVisible(true);
        m_folderDropHighlight->arrange(renderer, m_lastDropTarget->rect);
      } else if (slot) {
        m_placeDropLine->setVisible(true);
        m_placeDropLine->arrange(renderer, m_places->dropLine(*slot));
      }
      const bool copy = (m_connection.input().keyboardModifiers() & KeyMod::Ctrl) != 0;
      const auto shape = slot
          ? (m_drag->sourcePin ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY)
          : m_lastDropTarget ? (copy ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE)
                             : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP;
      m_connection.input().setCursorShape(m_connection.input().lastSerial(), shape);
    }
    void finishDrag(float x, float y) {
      m_pointerX = x;
      m_pointerY = y;
      const bool current = dragCurrent();
      const auto target = dropTarget();
      const auto previous = m_lastDropTarget;
      const auto slot = pinDropSlot();
      const auto drag = *m_drag;
      const bool copy = (m_connection.input().keyboardModifiers() & KeyMod::Ctrl) != 0;
      cancelDrag();
      if (!current) {
        return;
      }
      if (target && previous && target->location == previous->location && target->entry == previous->entry) {
        std::vector<files::Location> sources;
        sources.reserve(drag.entries.size());
        for (const auto& entry : drag.entries) {
          sources.push_back(entry.location);
        }
        m_actionError.clear();
        (void)m_transfers.start(
            {std::move(sources), target->location, copy ? files::TransferAction::COPY : files::TransferAction::MOVE}
        );
      } else if (slot) {
        pinFolders(drag.bookmarks, *slot);
      }
    }
    void clearTypeahead() {
      if (m_typeahead.text().empty() && m_typeaheadPreedit.empty())
        return;
      m_typeahead.clear();
      m_typeaheadPreedit.clear();
      m_typeaheadTimer.stop();
      m_browserDirty = true;
      m_surface.requestUpdate();
      m_textInput.notifyClientStateChanged(this, TextInputChangeCause::Other);
    }
    void
    typeaheadChanged(std::optional<files::EntryId> match, TextInputChangeCause cause = TextInputChangeCause::Other) {
      if (match)
        m_keyboardReveal = match;
      m_typeaheadContext = m_browser.contextSerial();
      m_fileKeyboardFocus = true;
      m_browserDirty = true;
      m_surface.requestUpdate();
      m_typeaheadTimer.start(files::TypeaheadSelection::timeout, [this] {
        if (m_typeaheadPreedit.empty())
          clearTypeahead();
      });
      m_textInput.notifyClientStateChanged(this, cause);
    }
    void applyTypeahead(
        const std::string& text, TextInputChangeCause cause = TextInputChangeCause::Other, bool composing = false
    ) {
      m_dispatcher.cancelPointerCapture();
      m_list->cancelPointerGesture();
      typeaheadChanged(
          m_typeahead.append(m_browser.model(), text, files::TypeaheadSelection::Clock::now(), composing), cause
      );
    }
    TextInputState textInputState() const override {
      if (m_typingMode == files::TypingMode::SEARCH)
        return m_filterInput->textInputState();
      const auto bounds = m_status ? sceneRect(*m_status) : LayoutRect{};
      return {
          .surroundingText = m_typeahead.text(),
          .cursor = static_cast<std::int32_t>(m_typeahead.text().size()),
          .anchor = static_cast<std::int32_t>(m_typeahead.text().size()),
          .cursorRectX = static_cast<int>(bounds.x),
          .cursorRectY = static_cast<int>(bounds.y),
          .cursorRectHeight = std::max(1, static_cast<int>(bounds.height))
      };
    }
    void textInputApplyEdit(const TextInputEdit& edit) override {
      if (m_typingMode == files::TypingMode::TYPEAHEAD) {
        const auto now = files::TypeaheadSelection::Clock::now();
        if (edit.hasDelete
            && edit.deleteBeforeLength != 0
            && edit.deleteAfterLength == 0
            && !m_typeahead.text().empty())
          typeaheadChanged(
              m_typeahead.erase(m_browser.model(), edit.deleteBeforeLength, now, !m_typeaheadPreedit.empty()),
              TextInputChangeCause::InputMethod
          );
        if (edit.hasCommitText && !edit.commitText.empty()) {
          const bool composing = !m_typeaheadPreedit.empty();
          m_typeaheadPreedit.clear();
          applyTypeahead(edit.commitText, TextInputChangeCause::InputMethod, composing);
        }
        if (edit.hasPreedit
            && (!edit.preeditText.empty() || !m_typeaheadPreedit.empty())
            && edit.preeditText.size() <= 256
            && g_utf8_validate(edit.preeditText.data(), static_cast<gssize>(edit.preeditText.size()), nullptr)) {
          m_typeaheadPreedit = edit.preeditText;
          m_typeaheadContext = m_browser.contextSerial();
          if (!m_typeaheadPreedit.empty())
            m_typeaheadTimer.stop();
          else
            typeaheadChanged(std::nullopt, TextInputChangeCause::InputMethod);
          m_browserDirty = true;
          m_surface.requestUpdate();
        }
        return;
      }
      if ((edit.hasCommitText && !edit.commitText.empty())
          || (edit.hasPreedit && !edit.preeditText.empty())
          || (edit.hasDelete && !m_browser.searchQuery().empty())) {
        openFilter(false, true);
        m_filterInput->textInputApplyEdit(edit);
      }
    }
    void textInputResetPreedit() override {
      if (!m_typeaheadPreedit.empty()) {
        m_typeaheadPreedit.clear();
        m_browserDirty = true;
        m_surface.requestUpdate();
      }
    }
    void textInputActivated(TextInputService&) override {}
    void textInputDeactivated(TextInputService&) override { clearTypeahead(); }

    void clearCompletions() {
      m_pathCompletion.cancel();
      m_completionText.clear();
      m_completionItems = {};
      m_completionIndex.reset();
      m_completionDirty = true;
      m_surface.requestLayout();
    }
    void requestCompletions(const std::string& text) {
      clearCompletions();
      if (!m_editingLocation || !m_browser.model().snapshot().location || m_location->composing())
        return;
      const auto edit = m_location->textInputState();
      if (edit.cursor != static_cast<std::int32_t>(text.size()) || edit.anchor != edit.cursor)
        return;
      const auto context = m_browser.contextSerial();
      m_pathCompletion.complete(
          text, *m_browser.model().snapshot().location, m_browser.model().filter().showHidden,
          [this, text, context](files::PathSuggestions suggestions) {
            if (!m_editingLocation
                || context != m_browser.contextSerial()
                || m_location->value() != text
                || m_location->composing())
              return;
            const auto edit = m_location->textInputState();
            if (edit.cursor != static_cast<std::int32_t>(text.size()) || edit.anchor != edit.cursor)
              return;
            m_completionText = text;
            m_completionContext = context;
            m_completionItems = std::move(suggestions);
            m_completionIndex.reset();
            m_completionDirty = true;
            m_surface.requestLayout();
          }
      );
    }
    void acceptCompletion(std::size_t index, bool navigate) {
      if (index >= m_completionItems.items.size())
        return;
      const auto suggestion = m_completionItems.items[index];
      clearCompletions();
      if (navigate) {
        m_browser.open(suggestion.location);
        endLocationEdit(true);
      } else {
        m_location->setValue(suggestion.text);
        m_location->selectRange(suggestion.text.size(), suggestion.text.size());
        m_dispatcher.setFocus(m_location->inputArea());
      }
    }
    bool completionKey(std::uint32_t key, std::uint32_t modifiers) {
      const auto edit = m_location->textInputState();
      if (m_location->composing()
          || m_completionContext != m_browser.contextSerial()
          || m_location->value() != m_completionText
          || edit.cursor != static_cast<std::int32_t>(m_completionText.size())
          || edit.anchor != edit.cursor) {
        clearCompletions();
        return false;
      }
      if (modifiers != 0 && (!KeySymbol::isTab(key) || modifiers != KeyMod::Shift))
        return false;
      if (m_completionItems.items.empty())
        return false;
      if (KeySymbol::isEscape(key)) {
        clearCompletions();
        return true;
      }
      if (KeySymbol::isTab(key)) {
        acceptCompletion(m_completionIndex.value_or(0), false);
        return true;
      }
      if (KeySymbol::isEnter(key) && m_completionIndex) {
        acceptCompletion(*m_completionIndex, true);
        return true;
      }
      if (KeySymbol::isDown(key) || KeySymbol::isUp(key)) {
        const auto count = m_completionItems.items.size();
        m_completionIndex = m_completionIndex
            ? (*m_completionIndex + count + (KeySymbol::isDown(key) ? 1 : count - 1)) % count
            : KeySymbol::isDown(key) ? 0
                                     : count - 1;
        m_completionDirty = true;
        m_surface.requestLayout();
        return true;
      }
      if (KeySymbol::isLeft(key) || KeySymbol::isRight(key) || KeySymbol::isHome(key) || KeySymbol::isEnd(key))
        clearCompletions();
      return false;
    }
    void layoutCompletions(Renderer& renderer) {
      if (m_completionDirty) {
        m_completionDirty = false;
        std::vector<ContextMenuControlEntry> entries;
        constexpr std::size_t pageSize = 6;
        const auto page = m_completionIndex.value_or(0) / pageSize * pageSize;
        for (auto i = page; i < std::min(page + pageSize, m_completionItems.items.size()); ++i)
          entries.push_back({.id = static_cast<int>(i), .label = m_completionItems.items[i].label});
        if (m_completionItems.items.size() > pageSize || m_completionItems.truncated)
          entries.push_back(
              {.label = m_completionItems.truncated ? "More matches; type to narrow" : "Arrow keys for more matches",
               .enabled = false}
          );
        if (!m_completionItems.error.empty() && entries.empty())
          entries.push_back({.label = "Folder suggestions unavailable", .enabled = false});
        const bool visible = m_editingLocation && !entries.empty() && !m_menuSurface;
        m_completionFrame->setVisible(visible);
        m_completionControl->setMaxVisible(entries.size());
        m_completionControl->setEntries(std::move(entries));
        if (m_completionIndex)
          m_completionControl->setHighlightedIndex(*m_completionIndex - page);
        else
          m_completionControl->setHighlightedIndex(m_completionControl->entryCount());
      }
      if (!m_completionFrame->visible())
        return;
      const auto anchor = sceneRect(*m_locationArea);
      const float width = std::min(anchor.width, static_cast<float>(m_surface.width()) - 16.0F);
      m_completionControl->setMenuWidth(width);
      m_completionFrame->arrange(
          renderer,
          {std::clamp(anchor.x, 8.0F, std::max(8.0F, static_cast<float>(m_surface.width()) - width - 8.0F)),
           anchor.y + anchor.height + 4.0F, width, m_completionControl->preferredHeight()}
      );
    }
    void beginLocationEdit() {
      m_blankLocationClick.reset();
      m_lastBreadcrumb.reset();
      m_breadcrumbEditTarget.reset();
      clearCompletions();
      m_editingLocation = true;
      m_locationInvalid = false;
      m_location->setValue(
          m_browser.model().snapshot().location ? files::locationEditText(*m_browser.model().snapshot().location)
                                                : std::string{}
      );
      m_location->setVisible(true);
      m_location->setParticipatesInLayout(true);
      m_breadcrumbScroll->setVisible(false);
      m_breadcrumbScroll->setParticipatesInLayout(false);
      m_dispatcher.setFocus(m_location->inputArea());
      m_location->selectAll();
      m_browserDirty = true;
      m_surface.requestUpdate();
    }
    void endLocationEdit(bool focusFiles) {
      clearCompletions();
      m_editingLocation = false;
      m_locationInvalid = false;
      m_location->setVisible(false);
      m_location->setParticipatesInLayout(false);
      m_breadcrumbScroll->setVisible(true);
      m_breadcrumbScroll->setParticipatesInLayout(true);
      if (focusFiles) {
        m_dispatcher.setFocus(m_list->focusArea());
      }
      m_browserDirty = true;
      m_surface.requestUpdate();
    }
    void breadcrumbClick(const files::Location& target, const InputArea::PointerData* pointer = nullptr) {
      m_lastBreadcrumb = pointer != nullptr ? std::optional(target) : std::nullopt;
      if (pointer != nullptr) {
        m_breadcrumbClickTime = std::chrono::steady_clock::now();
        m_breadcrumbClickX = pointer->sceneX;
        m_breadcrumbClickY = pointer->sceneY;
      }
      m_browser.open(target);
      m_dispatcher.setFocus(m_list->focusArea());
    }
    void openBreadcrumbMenu(const files::Location& target, LayoutRect anchor) {
      const auto context = m_browser.contextSerial();
      auto targets = std::make_shared<std::vector<files::Location>>();
      std::vector<ContextMenuControlEntry> entries;
      for (const auto& crumb : files::locationBreadcrumbs(target)) {
        const auto index = targets->size();
        targets->push_back(crumb.location);
        entries.push_back({.id = static_cast<int>(index), .label = crumb.label});
      }
      entries.push_back({.label = "", .separator = true});
      const auto request = actionRequest(false);
      for (const auto action : {Action::COPY_PATH, Action::COPY_URI, Action::TERMINAL}) {
        auto entry = actionEntry(action, request);
        entry.id = -1 - static_cast<int>(action);
        if (action == Action::TERMINAL)
          entry.enabled = target.nativePath().has_value() && m_terminal.available();
        entries.push_back(std::move(entry));
      }
      entries.push_back({.label = "", .separator = true});
      entries.push_back({.label = "Sibling folders", .header = true});
      entries.push_back({.label = "Loading folders…", .enabled = false});
      const auto activate = [this, context, target, targets](int id) {
        if (context != m_browser.contextSerial())
          return;
        if (id >= 0) {
          if (static_cast<std::size_t>(id) < targets->size())
            breadcrumbClick((*targets)[static_cast<std::size_t>(id)]);
          return;
        }
        const auto action = static_cast<Action>(-1 - id);
        if (action == Action::TERMINAL) {
          if (!target.nativePath() || !m_terminal.available())
            return;
          const std::weak_ptr<int> lifetime = m_lifetime;
          m_terminal.open(target, [this, lifetime, context](std::string error) {
            if (!lifetime.expired() && context == m_browser.contextSerial() && !error.empty())
              actionError(std::move(error));
          });
        } else if (m_clipboard.available())
          m_clipboard.setClipboardText(action == Action::COPY_URI ? target.uri() : files::locationEditText(target));
      };
      showContextMenu(entries, activate, anchor);
      const auto generation = m_menuGeneration;
      const files::GioPtr<GFile> parent(g_file_get_parent(target.file().get()));
      const files::GString uri(parent ? g_file_get_uri(parent.get()) : nullptr);
      const auto siblingParent = uri ? files::Location::fromUri(uri.get()) : target;
      m_siblingCompletion.children(
          siblingParent, m_browser.model().filter().showHidden,
          [this, context, generation, anchor, targets, entries = std::move(entries),
           activate](const files::PathSuggestions& suggestions) mutable {
            if (context != m_browser.contextSerial() || generation != m_menuGeneration || !m_menuSurface)
              return;
            entries.pop_back();
            for (const auto& item : suggestions.items) {
              entries.push_back({.id = static_cast<int>(targets->size()), .label = item.label});
              targets->push_back(item.location);
            }
            if (suggestions.items.empty())
              entries.push_back(
                  {.label = suggestions.error.empty() ? "No sibling folders" : "Folders unavailable", .enabled = false}
              );
            if (suggestions.truncated)
              entries.push_back({.label = "First 256 folders; open parent for all", .enabled = false});
            showContextMenu(std::move(entries), activate, anchor);
          }
      );
    }
    void rebuildBreadcrumbs() {
      m_dropBreadcrumbs.clear();
      while (!m_breadcrumbs->children().empty()) {
        m_breadcrumbs->removeChild(m_breadcrumbs->children().back().get());
      }
      const auto& location = m_browser.model().snapshot().location;
      if (!location) {
        return;
      }
      const auto crumbs = files::locationBreadcrumbs(*location);
      for (std::size_t i = 0; i < crumbs.size(); ++i) {
        if (i != 0) {
          m_breadcrumbs->addChild(
              ui::glyph(
                  {.glyph = "chevron-right",
                   .glyphSize = 12.0F,
                   .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
              )
          );
        }
        const auto target = crumbs[i].location;
        m_breadcrumbs->addChild(
            ui::button(
                {.text = crumbs[i].label,
                 .fontSize = Style::fontSizeCaption,
                 .controlHeight = Style::controlHeightSm,
                 .contentAlign = ButtonContentAlign::Start,
                 .variant = ButtonVariant::Ghost,
                 .maxWidth = 160.0F,
                 .paddingH = Style::spaceSm,
                 .onClick = [this, target] { breadcrumbClick(target); },
                 .configure =
                     [this, target](Button& button) {
                       m_dropBreadcrumbs.emplace_back(target, &button);
                       button.label()->setUseMarkup(false);
                       button.inputArea()->setAcceptedButtons(
                           InputArea::buttonMask(BTN_LEFT) | InputArea::buttonMask(BTN_RIGHT)
                       );
                       button.inputArea()->setOnClick([this, target](const InputArea::PointerData& pointer) {
                         if (pointer.button == BTN_LEFT) {
                           breadcrumbClick(target, &pointer);
                         } else if (pointer.button == BTN_RIGHT)
                           openBreadcrumbMenu(target, {pointer.sceneX, pointer.sceneY, 1, 1});
                       });
                     }}
            )
        );
      }
      m_breadcrumbScroll->requestScrollToBottom();
    }

    void openFilter(bool selectAll = true, bool preserveTextInput = false) {
      m_blankLocationClick.reset();
      clearTypeahead();
      m_lastBreadcrumb.reset();
      m_breadcrumbEditTarget.reset();
      if (m_editingLocation) {
        endLocationEdit(false);
      }
      m_filterOpen = true;
      if (!m_filterInput->composing() && m_filterInput->value() != m_browser.searchQuery()) {
        m_filterInput->setValue(m_browser.searchQuery());
      }
      m_dispatcher.setFocus(m_filterInput->inputArea(), preserveTextInput);
      if (selectAll) {
        m_filterInput->selectAll();
      }
      m_browserDirty = true;
      m_surface.requestUpdate();
    }

    void changeNameFilter(const std::string& text) {
      m_filterOpen = true;
      m_browser.setSearchComposing(m_filterInput->composing());
      if (text == m_browser.searchQuery()) {
        return;
      }
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      m_viewAnchor.reset();
      m_list->scrollView().requestScrollToOffset(0);
      m_browser.editSearchQuery(text);
    }

    void closeFilter() {
      m_filterOpen = false;
      changeNameFilter({});
      m_filterOpen = false;
      m_filterInput->setValue({});
      m_dispatcher.setFocus(m_list->focusArea());
      m_browserDirty = true;
      m_surface.requestUpdate();
    }

    static constexpr float kSidebarHandleWidth = 6.0F;

    void setSidebarVisible(bool visible, bool persist = true) {
      if (visible == m_showSidebar)
        return;
      m_dispatcher.cancelPointerCapture();
      if (m_drag)
        cancelDrag();
      captureViewAnchor();
      m_showSidebar = visible;
      if (!visible) {
        m_restorePlaceFocus.reset();
        for (const Node* node = m_dispatcher.focusedArea(); node != nullptr; node = node->parent()) {
          if (node == m_places || node == m_sidebarResize) {
            m_dispatcher.setFocus(m_list->focusArea());
            break;
          }
        }
      }
      if (persist)
        m_preferences.saveShowSidebar(visible);
      m_surface.requestLayout();
    }

    int sidebarMaximumWidth() const {
      return std::clamp(
          static_cast<int>(m_surface.width()) - 320 - static_cast<int>(kSidebarHandleWidth), files::kMinSidebarWidth,
          files::kMaxSidebarWidth
      );
    }

    void resizeSidebar(int width, bool persist) {
      const int bounded = std::clamp(width, files::kMinSidebarWidth, sidebarMaximumWidth());
      if (bounded == m_sidebarWidth)
        return;
      captureViewAnchor();
      m_sidebarWidth = bounded;
      if (persist)
        m_preferences.saveSidebarWidth(m_sidebarWidth);
      m_surface.requestLayout();
    }

    void captureViewAnchor() {
      if (m_list == nullptr) {
        return;
      }
      const auto first = m_list->firstVisibleIndex();
      const auto& rows = m_browser.model().rows();
      if (first && *first < rows.size()) {
        m_viewAnchor = rows[*first];
      }
    }

    void setDetailsColumnVisibility(files::DetailsColumnVisibility visibility) {
      if (visibility == m_columnVisibility)
        return;
      captureViewAnchor();
      m_dispatcher.cancelPointerCapture();
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      m_columnVisibility = visibility;
      m_preferences.saveDetailsColumns(visibility);
      m_browserDirty = true;
      m_surface.requestUpdate();
    }

    struct ColumnResize {
      std::size_t index;
      float pressX;
      float width;
      files::DetailsColumnWidths before;
      float offset;
      int nameWidth;
    };

    Button* widthHeader(std::size_t index) const {
      return index == 0 ? m_nameHeader : columnHeader(static_cast<files::DetailsColumn>(index - 1));
    }

    float headerWidth(std::size_t index) const { return widthHeader(index)->width(); }

    void cancelColumnResize() {
      if (!m_columnResize)
        return;
      m_columnWidths = m_columnResize->before;
      m_detailsOffset = m_columnResize->offset;
      m_columnResize.reset();
      m_columnHoverDirty = true;
      m_surface.requestLayout();
    }

    void setDetailsColumnWidths(files::DetailsColumnWidths widths) {
      if (m_columnWidths == widths)
        return;
      captureViewAnchor();
      m_dispatcher.cancelPointerCapture();
      m_list->cancelPointerGesture();
      m_columnWidths = widths;
      m_preferences.saveColumnWidths(widths);
      m_surface.requestLayout();
    }

    void scrollDetails(float offset) {
      if (m_viewMode != files::ViewMode::LIST || m_columnResize || m_columnDrag)
        return;
      const float maximum = std::max(0.0F, m_detailsColumns.contentWidth - m_detailsHeaderViewport->width());
      const float next = std::clamp(offset, 0.0F, maximum);
      if (next == m_detailsOffset)
        return;
      m_list->cancelPointerGesture();
      m_detailsOffset = next;
      m_columnHoverDirty = true;
      m_surface.requestLayout();
    }

    void layoutDetails(Renderer& renderer) {
      const bool list = m_viewMode == files::ViewMode::LIST;
      // The header remains laid out when the file grid is hidden for an empty
      // folder or an error. Do not derive its width from that stale grid.
      const float viewport = list ? m_detailsHeaderViewport->width() : m_list->scrollView().contentViewportWidth(true);
      const auto columns = files::DetailsColumns::forWidth(viewport, m_columnVisibility, m_columnWidths);
      const bool columnsChanged = columns != m_detailsColumns;
      m_detailsColumns = columns;
      const bool overflow = list && columns.contentWidth > viewport + 0.5F;
      m_detailsScrollHost->setVisible(overflow);
      m_detailsScrollHost->setParticipatesInLayout(overflow);
      for (std::size_t index = 0; index < 4; ++index) {
        auto* header = widthHeader(index);
        const bool visible =
            index == 0 || files::detailsColumnVisible(m_columnVisibility, static_cast<files::DetailsColumn>(index - 1));
        header->setVisible(visible);
        header->setParticipatesInLayout(visible);
        const float allocation = index == 0 ? columns.nameWidth : static_cast<float>(m_columnWidths[index]);
        header->setMinWidth(allocation);
        header->setMaxWidth(allocation);
        if (!visible && m_dispatcher.focusedArea() == header->inputArea())
          m_dispatcher.setFocus(m_nameHeader->inputArea());
      }
      m_detailsOffset = std::clamp(m_detailsOffset, 0.0F, std::max(0.0F, columns.contentWidth - viewport));
      m_listAdapter.setDetailsColumns(columns);
      m_list->setHorizontalOverflow(list ? columns.contentWidth : 0.0F, list ? m_detailsOffset : 0.0F);
      if (columnsChanged)
        m_list->notifyDataChanged();
      m_root->arrange(renderer, {0, 0, static_cast<float>(m_surface.width()), static_cast<float>(m_surface.height())});
      m_detailsHeaderRow->arrange(
          renderer, {-m_detailsOffset, 0, std::max(viewport, columns.contentWidth), m_detailsHeaderViewport->height()}
      );
      // Reveal focused headers without moving the user's horizontal position
      // on every frame. Pointer resizing retains its own captured offset.
      auto* focused = m_dispatcher.focusedArea();
      Button* reveal = std::exchange(m_revealDetailsHeader, nullptr);
      if (focused != m_lastDetailsFocus && !m_columnResize && !m_columnDrag)
        for (std::size_t index = 0; index < 4; ++index)
          if (widthHeader(index)->inputArea() == focused)
            reveal = widthHeader(index);
      m_lastDetailsFocus = focused;
      if (list && reveal && reveal->visible()) {
        const auto bounds = sceneRect(*reveal);
        const auto clip = sceneRect(*m_detailsHeaderViewport);
        const float delta = bounds.width > clip.width
            ? (Style::rtl() ? bounds.x + bounds.width - clip.x - clip.width : bounds.x - clip.x)
            : bounds.x < clip.x                             ? bounds.x - clip.x
            : bounds.x + bounds.width > clip.x + clip.width ? bounds.x + bounds.width - clip.x - clip.width
                                                            : 0.0F;
        if (delta != 0.0F)
          scrollDetails(m_detailsOffset + delta);
      }
      m_detailsScrollbar->update(viewport, columns.contentWidth, m_detailsOffset);
      m_detailsScrollbar->arrange(renderer, {0, 0, viewport, Style::scrollbarWidth});
      for (std::size_t index = 0; index < 4; ++index) {
        auto* handle = m_columnResizeHandles[index];
        auto* header = widthHeader(index);
        handle->setVisible(list && header->visible());
        const float edge = header->x() + (Style::rtl() ? 0.0F : header->width()) - m_detailsOffset;
        handle->arrange(renderer, {edge - 4.0F, 0, 8.0F, m_detailsHeaderViewport->height()});
        auto* line = m_columnResizeLines[index];
        line->setVisible(handle->hovered() || (m_columnResize && m_columnResize->index == index));
        line->arrange(renderer, {3.5F, 4.0F, 1.0F, std::max(0.0F, handle->height() - 8.0F)});
      }
    }

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

    std::optional<ColumnDrop> columnDropAt(float x, float y) const {
      if (!m_columnDrag || !m_listHeader->visible() || !m_detailsHeaderViewport->containsScenePoint(x, y))
        return std::nullopt;
      const auto source = m_columnDrag->source;
      if (!columnHeader(source)->visible())
        return std::nullopt;
      std::vector<std::pair<files::DetailsColumn, LayoutRect>> visible;
      auto left = static_cast<float>(m_surface.width());
      float right = 0;
      for (const auto column : m_columnOrder) {
        auto* header = columnHeader(column);
        if (!header->visible())
          continue;
        const auto rect = sceneRect(*header);
        left = std::min(left, rect.x);
        right = std::max(right, rect.x + rect.width);
        if (column != source)
          visible.emplace_back(column, rect);
      }
      const auto bounds = sceneRect(*columnHeader(source));
      if (visible.empty() || x < left || x > right || y < bounds.y || y > bounds.y + bounds.height)
        return std::nullopt;
      std::optional<files::DetailsColumn> before;
      float marker = Style::rtl() ? visible.back().second.x : visible.back().second.x + visible.back().second.width;
      for (const auto& [column, rect] : visible) {
        if (Style::rtl() ? x > rect.x + rect.width * 0.5F : x < rect.x + rect.width * 0.5F) {
          before = column;
          marker = Style::rtl() ? rect.x + rect.width : rect.x;
          break;
        }
      }
      const auto order = files::insertDetailsColumnBefore(m_columnOrder, source, before);
      if (!order)
        return std::nullopt;
      return ColumnDrop{*order, {marker - 1.0F, bounds.y, 2.0F, bounds.height}};
    }

    void cancelColumnDrag() {
      const auto drag = std::exchange(m_columnDrag, std::nullopt);
      if (!drag)
        return;
      auto* header = columnHeader(drag->source);
      header->setCursorShape(drag->cursor);
      header->setHoverSuppressed(false);
      header->setOpacity(1.0F);
      if (m_dispatcher.focusedArea() == header->inputArea())
        m_dispatcher.setFocus(m_list->focusArea());
      m_columnHoverDirty = true;
      m_surface.requestLayout();
    }

    void updateColumnDrag() {
      if (!m_columnDrag)
        return;
      if (!m_columnDrag->active
          && std::hypot(m_pointerX - m_columnDrag->pressX, m_pointerY - m_columnDrag->pressY) < 6.0F)
        return;
      m_columnDrag->active = true;
      auto* header = columnHeader(m_columnDrag->source);
      header->setHoverSuppressed(true);
      header->setCursorShape(
          columnDropAt(m_pointerX, m_pointerY) ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING
                                               : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP
      );
      m_surface.requestLayout();
    }

    void finishColumnDrag(float x, float y) {
      const auto drop = columnDropAt(x, y);
      m_dispatcher.cancelPointerCapture();
      cancelColumnDrag();
      if (drop)
        setDetailsColumnOrder(drop->order);
      m_dispatcher.setFocus(m_list->focusArea());
      m_dispatcher.syncPointerHover();
      m_surface.requestLayout();
    }

    void layoutColumnDrag(Renderer& renderer) {
      const bool active = m_columnDrag && m_columnDrag->active;
      for (const auto column : m_columnOrder)
        columnHeader(column)->setOpacity(active && m_columnDrag->source == column ? 0.65F : 1.0F);
      const auto drop = active ? columnDropAt(m_pointerX, m_pointerY) : std::nullopt;
      const bool changed = drop && drop->order != m_columnOrder;
      m_columnDropLine->setVisible(changed);
      if (changed)
        m_columnDropLine->arrange(renderer, drop->marker);
    }

    Button* columnHeader(files::DetailsColumn column) const {
      switch (column) {
      case files::DetailsColumn::TYPE:
        return m_typeHeader;
      case files::DetailsColumn::SIZE:
        return m_sizeHeader;
      case files::DetailsColumn::MODIFIED:
        return m_modifiedHeader;
      }
      return nullptr;
    }

    std::optional<files::DetailsColumnOrder> movedColumnOrder(files::DetailsColumn column, int direction) const {
      return files::moveDetailsColumn(
          m_columnOrder, column, direction,
          {m_detailsColumns.showType, m_detailsColumns.showSize, m_detailsColumns.showModified}
      );
    }

    void setDetailsColumnOrder(files::DetailsColumnOrder order) {
      if (order == m_columnOrder || !files::validDetailsColumnOrder(order))
        return;
      captureViewAnchor();
      m_dispatcher.cancelPointerCapture();
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      m_columnOrder = order;
      m_preferences.saveColumnOrder(order);
      m_browserDirty = true;
      m_surface.requestUpdate();
    }

    void changeSort(files::SortOptions options) {
      if (options == m_browser.model().sort()) {
        return;
      }
      captureViewAnchor();
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      m_browser.model().setSort(options);
      m_preferences.saveSort(options);
      m_dispatcher.setFocus(m_list->focusArea());
    }

    void sortBy(files::SortField field) {
      auto options = m_browser.model().sort();
      options.descending = options.field == field && !options.descending;
      options.field = field;
      const auto action = field == files::SortField::NAME ? Action::SORT_NAME
          : field == files::SortField::TYPE               ? Action::SORT_TYPE
          : field == files::SortField::SIZE               ? Action::SORT_SIZE
                                                          : Action::SORT_MODIFIED;
      if (m_browser.model().sort().field == field)
        dispatchAction(options.descending ? Action::DESCENDING : Action::ASCENDING);
      else
        dispatchAction(action);
    }

    void setViewMode(files::ViewMode mode) {
      if (mode == m_viewMode) {
        return;
      }
      m_viewAnchor.reset();
      if (m_list != nullptr) {
        captureViewAnchor();
        m_dispatcher.setFocus(m_list->focusArea());
      }
      m_viewMode = mode;
      m_preferences.save(mode);
      m_browser.setDefaultViewMode(m_viewMode);
      m_browserDirty = true;
      m_surface.requestUpdate();
    }

    void applyViewMode() {
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      const bool grid = m_viewMode == files::ViewMode::GRID;
      m_list->setAdapter(grid ? &m_gridAdapter : &m_listAdapter);
      m_list->setColumns(grid ? 0 : 1);
      m_list->setMinCellWidth(std::max(88.0F, gridIconSize() + 56.0F));
      m_list->setCellHeight(gridHeight());
      m_list->setColumnGap(grid ? Style::spaceXs : 0.0F);
      m_list->setRowGap(grid ? Style::spaceXs : 2.0F);
      m_appliedViewMode = m_viewMode;
    }

    float gridHeight() const {
      const auto& location = m_browser.model().snapshot().location;
      const bool context = !m_browser.searchQuery().empty() || (location && location->isTrash());
      return m_viewMode == files::ViewMode::GRID ? gridIconSize() + (context ? 72.0F : 52.0F)
                                                 : (context ? 52.0F : 36.0F);
    }

    float gridIconSize() const { return static_cast<float>(files::kGridIconSizes[m_gridZoom]); }

    void changeGridZoom(int requested) {
      const auto zoom =
          static_cast<std::size_t>(std::clamp(requested, 0, static_cast<int>(files::kGridIconSizes.size()) - 1));
      if (zoom == m_gridZoom)
        return;
      captureViewAnchor();
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      m_gridZoom = zoom;
      m_preferences.saveGridIconSize(files::kGridIconSizes[m_gridZoom]);
      m_browser.setDefaultIconSize(static_cast<int>(gridIconSize()));
      m_browserDirty = true;
      m_surface.requestUpdate();
    }

    bool zoomFromKey(const KeyboardEvent& event) {
      if (m_viewMode != files::ViewMode::GRID || m_list == nullptr)
        return false;
      int zoom = static_cast<int>(m_gridZoom);
      const bool ctrl = event.modifiers == KeyMod::Ctrl;
      const bool ctrlShift = event.modifiers == (KeyMod::Ctrl | KeyMod::Shift);
      if (!ctrl && !ctrlShift)
        return false;
      if (event.sym == XKB_KEY_plus || event.sym == XKB_KEY_KP_Add || (ctrl && event.sym == XKB_KEY_equal)) {
        ++zoom;
      } else if (ctrl && (event.sym == XKB_KEY_minus || event.sym == XKB_KEY_KP_Subtract)) {
        --zoom;
      } else if (ctrl && (event.sym == XKB_KEY_0 || event.sym == XKB_KEY_KP_0)) {
        zoom = static_cast<int>(
            std::ranges::find(files::kGridIconSizes, files::kDefaultGridIconSize) - files::kGridIconSizes.begin()
        );
      } else {
        return false;
      }
      m_zoomScroll = 0;
      changeGridZoom(zoom);
      return true;
    }

    bool zoomFromScroll(const PointerEvent& event) {
      if (m_viewMode != files::ViewMode::GRID
          || m_list == nullptr
          || !m_list->visible()
          || m_connection.input().keyboardModifiers() != KeyMod::Ctrl
          || event.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
        m_zoomScroll = 0;
        return false;
      }
      float localX = 0, localY = 0;
      if (!Node::mapFromScene(m_list, static_cast<float>(event.sx), static_cast<float>(event.sy), localX, localY)
          || localX < 0
          || localY < 0
          || localX >= m_list->width()
          || localY >= m_list->height()) {
        return false;
      }
      const double steps = event.axisValue120 != 0 ? event.axisValue120 / 120.0
          : event.axisDiscrete != 0                ? static_cast<double>(event.axisDiscrete)
                                                   : event.axisValue / 15.0;
      if (!std::isfinite(steps)) {
        return true;
      }
      const auto limit = static_cast<double>(files::kGridIconSizes.size());
      m_zoomScroll = std::clamp(m_zoomScroll - steps, -limit, limit);
      const auto delta = static_cast<int>(m_zoomScroll);
      m_zoomScroll -= delta;
      changeGridZoom(static_cast<int>(m_gridZoom) + delta);
      return true;
    }

    bool listKey(const KeyboardEvent& event) {
      auto& model = m_browser.model();
      if (event.sym == XKB_KEY_a && event.modifiers == KeyMod::Ctrl) {
        dispatchAction(Action::SELECT_ALL);
        return true;
      }
      if (event.modifiers == 0 && KeySymbol::isEscape(event.sym)) {
        dispatchAction(Action::CLEAR_SELECTION);
        return true;
      }
      if ((event.sym == XKB_KEY_i || event.sym == XKB_KEY_I) && event.modifiers == (KeyMod::Ctrl | KeyMod::Shift)) {
        if (!event.repeat)
          dispatchAction(Action::INVERT_SELECTION);
        return true;
      }
      if (event.sym == XKB_KEY_s && event.modifiers == KeyMod::Ctrl) {
        if (!event.repeat)
          dispatchAction(Action::SELECT_PATTERN);
        return true;
      }
      if (event.modifiers == 0 && KeySymbol::isEnter(event.sym)) {
        if (!event.repeat
            && model.selection().focused
            && (m_fileKeyboardFocus || model.selection().selected.contains(*model.selection().focused))) {
          auto request = actionRequest();
          if (request.targets.empty()) {
            const auto* entry = model.entry(*model.selection().focused);
            if (entry)
              request.targets.push_back({entry->id, entry->location});
          }
          dispatchAction(Action::OPEN, request);
        }
        return true;
      }
      if (model.rows().empty() || (event.modifiers & (KeyMod::Alt | KeyMod::Super)) != 0) {
        return false;
      }
      auto index = std::int64_t{0};
      if (model.selection().focused) {
        const auto found = std::ranges::find(model.rows(), *model.selection().focused);
        if (found != model.rows().end()) {
          index = std::distance(model.rows().begin(), found);
        }
      }
      const bool hasFocus = model.selection().focused.has_value();
      const auto columns = static_cast<std::int64_t>(std::max<std::size_t>(1, m_list->layoutColumnCount()));
      if (event.sym == XKB_KEY_Down) {
        index += hasFocus ? columns : 0;
      } else if (event.sym == XKB_KEY_Up) {
        index += hasFocus ? -columns : 0;
      } else if (m_viewMode == files::ViewMode::GRID && event.sym == XKB_KEY_Right) {
        index += hasFocus ? (Style::rtl() ? -1 : 1) : 0;
      } else if (m_viewMode == files::ViewMode::GRID && event.sym == XKB_KEY_Left) {
        index += hasFocus ? (Style::rtl() ? 1 : -1) : 0;
      } else if (event.sym == XKB_KEY_Home) {
        index = 0;
      } else if (event.sym == XKB_KEY_End) {
        index = static_cast<std::int64_t>(model.rows().size()) - 1;
      } else if (event.sym == XKB_KEY_Page_Down) {
        index += static_cast<std::int64_t>(m_list->pageItemStride());
      } else if (event.sym == XKB_KEY_Page_Up) {
        index -= static_cast<std::int64_t>(m_list->pageItemStride());
      } else {
        return false;
      }
      const auto row = static_cast<std::size_t>(
          std::clamp(index, std::int64_t{0}, static_cast<std::int64_t>(model.rows().size()) - 1)
      );
      const auto id = model.rows()[row];
      const bool ctrl = (event.modifiers & KeyMod::Ctrl) != 0;
      const bool shift = (event.modifiers & KeyMod::Shift) != 0;
      if (ctrl && !shift) {
        (void)model.setFocus(id);
      } else {
        (void)model.select(
            id,
            shift ? (ctrl ? files::SelectionMode::ADD_RANGE : files::SelectionMode::RANGE)
                  : files::SelectionMode::REPLACE
        );
      }
      m_keyboardReveal = id;
      return true;
    }

    void syncBrowser() {
      if (m_typeaheadContext != m_browser.contextSerial() || !m_browser.model().selectionAvailable())
        clearTypeahead();
      const auto& model = m_browser.model();
      const auto& snapshot = model.snapshot();
      if (m_visitSerial != m_browser.visitSerial()) {
        m_visitSerial = m_browser.visitSerial();
        const auto& viewport = m_browser.viewport();
        m_viewMode = viewport.mode;
        m_gridZoom = static_cast<std::size_t>(
            std::ranges::find(files::kGridIconSizes, viewport.iconSize) - files::kGridIconSizes.begin()
        );
        m_detailsOffset = 0;
        m_keyboardReveal.reset();
      }
      const auto uri = snapshot.location ? snapshot.location->uri() : std::string{};
      if (uri != m_displayedLocation) {
        clearCompletions();
        m_siblingCompletion.cancel();
        if (snapshot.location != m_lastBreadcrumb) {
          m_lastBreadcrumb.reset();
        }
        m_filterOpen = false;
        if (m_dispatcher.focusedArea() == m_filterInput->inputArea() || m_editingLocation) {
          m_dispatcher.setFocus(m_list->focusArea());
        }
        m_editingLocation = false;
        m_location->setVisible(false);
        m_location->setParticipatesInLayout(false);
        m_breadcrumbScroll->setVisible(true);
        m_breadcrumbScroll->setParticipatesInLayout(true);
        m_breadcrumbsDirty = true;
        m_displayedLocation = uri;
        m_location->setValue(snapshot.location ? files::locationEditText(*snapshot.location) : std::string{});
        m_locationInvalid = false;
        m_listAdapter.onPointerCancel();
        m_gridAdapter.onPointerCancel();
        m_viewAnchor.reset();
        m_list->scrollView().requestScrollToOffset(0);
      }
      if (auto restored = m_browser.takeRestoredViewport()) {
        m_pendingHistoryViewport = restored;
        m_viewAnchor.reset();
      }
      m_location->setInvalid(m_locationInvalid);
      if (!m_filterInput->composing() && m_filterInput->value() != m_browser.searchQuery()) {
        m_filterInput->setValue(m_browser.searchQuery());
      }
      const bool searchFocused = m_dispatcher.focusedArea() == m_filterInput->inputArea();
      m_searchArea->setBorder(
          colorSpecFromRole(searchFocused ? ColorRole::Primary : ColorRole::OutlineVariant),
          searchFocused ? Style::focusRingWidth : Style::borderWidth
      );
      m_locationArea->setBorder(
          colorSpecFromRole(m_locationInvalid ? ColorRole::Error : ColorRole::OutlineVariant), Style::borderWidth
      );
      m_list->setCellHeight(gridHeight());
      m_list->setMinCellWidth(std::max(88.0F, gridIconSize() + 56.0F));
      m_gridAdapter.setGridIconSize(gridIconSize());
      m_listAdapter.setDetailsColumns(m_detailsColumns);
      m_listAdapter.setDetailsColumnOrder(m_columnOrder);
      const auto& sort = model.sort();
      const char* indicator = sort.descending ? "arrow-down" : "arrow-up";
      m_nameHeader->setGlyph(indicator);
      m_sizeHeader->setGlyph(indicator);
      m_modifiedHeader->setGlyph(indicator);
      m_typeHeader->setGlyph(indicator);
      m_nameHeader->glyph()->setVisible(sort.field == files::SortField::NAME);
      m_nameHeader->glyph()->setParticipatesInLayout(sort.field == files::SortField::NAME);
      m_sizeHeader->glyph()->setVisible(sort.field == files::SortField::SIZE);
      m_sizeHeader->glyph()->setParticipatesInLayout(sort.field == files::SortField::SIZE);
      m_modifiedHeader->glyph()->setVisible(sort.field == files::SortField::MODIFIED);
      m_modifiedHeader->glyph()->setParticipatesInLayout(sort.field == files::SortField::MODIFIED);
      m_typeHeader->glyph()->setVisible(sort.field == files::SortField::TYPE);
      m_typeHeader->glyph()->setParticipatesInLayout(sort.field == files::SortField::TYPE);
      const char* field = sort.field == files::SortField::NAME ? "Name"
          : sort.field == files::SortField::SIZE               ? "Size"
          : sort.field == files::SortField::TYPE               ? "Type"
                                                               : "Modified";
      const char* direction = sort.field == files::SortField::MODIFIED
          ? (sort.descending ? "newest first" : "oldest first")
          : sort.field == files::SortField::SIZE ? (sort.descending ? "largest first" : "smallest first")
                                                 : (sort.descending ? "Z to A" : "A to Z");
      m_menuButton->setTooltip(
          std::format("View options: {}, {}{}", field, direction, sort.directoriesFirst ? " · Folders first" : "")
      );
      const bool grid = m_viewMode == files::ViewMode::GRID;
      m_viewSwitch->setSelectedIndex(grid ? 0 : 1);
      m_listHeader->setVisible(!grid);
      m_listHeader->setParticipatesInLayout(!grid);
      const auto request = actionRequest(false);
      for (const auto& [button, action] : std::array{
               std::pair{m_back, Action::BACK}, std::pair{m_forward, Action::FORWARD}, std::pair{m_up, Action::UP}
           }) {
        const auto state = actionState(action, request);
        button->setEnabled(state.enabled);
        const auto& definition = files::ActionRegistry::definition(action);
        button->setTooltip(
            std::string(definition.label)
            + " ("
            + std::string(definition.shortcut)
            + ")"
            + (state.enabled ? "" : "\n" + std::string(state.reason))
        );
      }
      m_places->sync(snapshot.location);
      const bool loading = snapshot.status == files::DirectoryStatus::LOADING;
      m_refresh->setGlyph(loading ? "square" : "refresh");
      m_refresh->setTooltip(loading ? "Stop loading" : "Refresh");
      std::string status;
      if (m_creator.busy()) {
        const char* kind = m_creator.snapshot().kind == files::CreateKind::FOLDER ? "folder" : "file";
        status = m_creator.snapshot().status == files::CreateStatus::CREATING
            ? std::format("Creating {}…", kind)
            : std::format("Preparing {} creation…", kind);
      } else if (m_renamer.busy()) {
        status = m_renamer.snapshot().status == files::RenameStatus::RENAMING ? "Renaming…" : "Preparing rename…";
      } else if (m_emptyTrash.busy()) {
        status = m_emptyTrash.snapshot().status == files::EmptyTrashStatus::PREPARING ? "Reading Trash…"
            : m_emptyTrash.snapshot().status == files::EmptyTrashStatus::READY
            ? "Calculating Trash size…"
            : std::format(
                  "Emptying Trash… · {} of {} items", m_emptyTrash.completed(), m_emptyTrash.snapshot().results.size()
              );
      } else if (m_trash.busy()) {
        status = m_trash.snapshot().status == files::TrashStatus::PREPARING
            ? "Preparing move to Trash…"
            : std::format("Moving to Trash… · {} of {} items", m_trash.completed(), m_trash.snapshot().results.size());
      } else if (m_transfers.busy()) {
        const files::GString size(g_format_size(m_transfers.bytes()));
        status = std::format(
            "{} {} of {} items · {}", m_transfers.action() == files::TransferAction::COPY ? "Copying" : "Moving",
            m_transfers.completed(), m_transfers.total(), size.get()
        );
      } else if (!m_actionError.empty()) {
        status = m_actionError;
      } else if (!m_pins.error().empty()) {
        status = std::format("Could not update pins: {}", m_pins.error());
      } else if (!m_preferenceError.empty()) {
        status = m_preferenceError;
      } else if (!m_browser.message().empty()) {
        status = m_browser.message();
      } else if (snapshot.error) {
        status = snapshot.error->message;
      } else if (!m_browser.searchQuery().empty()) {
        status = std::format(
            "{}{} results",
            loading                                                    ? "Searching… · "
                : snapshot.status == files::DirectoryStatus::CANCELLED ? "Search stopped · "
                                                                       : "",
            model.rows().size()
        );
        if (m_browser.skippedSearchFolders() != 0) {
          status += std::format(" · {} folders could not be searched", m_browser.skippedSearchFolders());
        }
      } else if (loading) {
        status = "Loading folder…";
      } else if (snapshot.status == files::DirectoryStatus::CANCELLED) {
        status = "Loading stopped.";
      } else {
        const auto hidden = model.filter().showHidden
            ? 0U
            : static_cast<std::size_t>(std::ranges::count_if(snapshot.entries, [](const auto& entry) {
                return entry.hidden;
              }));
        status = std::format("{} items", model.rows().size());
        if (hidden > 0) {
          status += std::format(" · {} hidden", hidden);
        }
      }
      if (snapshot.location && snapshot.location->isTrash() && !snapshot.error && !loading)
        status += " · Refresh to update";
      if (!model.selection().selected.empty()) {
        status += std::format(" · {} selected", model.selection().selected.size());
        const auto summary = model.selectionSummary();
        if (summary.overflow) {
          status += " · file size exceeds limit";
        } else if (summary.files != 0 && summary.unknownSizes == summary.files) {
          status += " · file size unknown";
        } else if (summary.files != 0) {
          const files::GString size(g_format_size_full(summary.bytes, G_FORMAT_SIZE_IEC_UNITS));
          status += std::format(
              " · {}{}{}", summary.unknownSizes == 0 ? "" : "at least ", size.get(),
              summary.folders != 0 || summary.others != 0 ? " in files" : ""
          );
        }
      }
      if (!m_typeahead.text().empty() || !m_typeaheadPreedit.empty())
        status = "Select: " + m_typeahead.text() + m_typeaheadPreedit + " · " + status;
      m_status->setText(status);
      const auto freeBytes = snapshot.metadata.filesystem.freeBytes;
      const bool showFree = freeBytes
          && !snapshot.metadataLoading
          && !snapshot.filesystemError
          && !snapshot.error
          && snapshot.location
          && !snapshot.location->isTrash();
      if (showFree) {
        const files::GString size(g_format_size_full(*freeBytes, G_FORMAT_SIZE_IEC_UNITS));
        m_freeSpace->setText(std::format("{} free", size.get()));
      }
      m_freeSpace->setVisible(showFree);
      m_freeSpace->setParticipatesInLayout(showFree);
      const bool cancellable = m_transfers.busy() || m_trash.busy() || m_emptyTrash.busy();
      m_cancelOperation->setVisible(cancellable);
      m_cancelOperation->setParticipatesInLayout(cancellable);
      m_cancelOperation->setTooltip(
          m_emptyTrash.busy()  ? "Cancel emptying Trash"
              : m_trash.busy() ? "Cancel move to Trash"
                               : "Cancel transfer"
      );
      m_notice->setText(
          snapshot.error
                  || loading
                  || snapshot.status == files::DirectoryStatus::CANCELLED
                  || !m_browser.message().empty()
                  || !m_preferenceError.empty()
              ? status
              : !m_browser.searchQuery().empty() ? "No matching files or folders."
              : snapshot.entries.empty()
              ? (snapshot.location == files::Location::trash() ? "Trash is empty." : "This folder is empty.")
              : "No visible items."
      );
      const bool empty = model.rows().empty();
      m_noticeArea->setVisible(empty);
      m_noticeArea->setParticipatesInLayout(empty);
      m_list->setVisible(!empty);
      m_list->setParticipatesInLayout(!empty);
      std::optional<std::size_t> focus;
      if (model.selection().focused) {
        const auto found = std::ranges::find(model.rows(), *model.selection().focused);
        if (found != model.rows().end()) {
          focus = static_cast<std::size_t>(std::distance(model.rows().begin(), found));
        }
      }
      m_list->setSelectedIndex(focus, false);
      m_list->notifyDataChanged();
      if (const auto reveal = std::exchange(m_keyboardReveal, std::nullopt);
          reveal && model.selection().focused == reveal) {
        const auto found = std::ranges::find(model.rows(), *reveal);
        if (found != model.rows().end())
          m_list->scrollToIndex(static_cast<std::size_t>(found - model.rows().begin()));
      }
      if (m_revealTarget && snapshot.status != files::DirectoryStatus::LOADING) {
        const auto target = *std::exchange(m_revealTarget, std::nullopt);
        const auto found = std::ranges::find_if(model.rows(), [&](files::EntryId id) {
          const auto* entry = model.entry(id);
          return entry != nullptr && entry->location == target;
        });
        if (found != model.rows().end()) {
          m_browser.model().select(*found);
          m_list->scrollToIndex(static_cast<std::size_t>(found - model.rows().begin()));
        }
      }
    }

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
        UiPhaseScope phase(UiPhase::Layout);
        buildScene();
      }
      const auto& thumbnailSnapshot = m_browser.model().snapshot();
      if (m_thumbnailContext != m_browser.contextSerial()
          || (thumbnailSnapshot.generation != m_thumbnailRevision
              && (thumbnailSnapshot.status == files::DirectoryStatus::READY
                  || thumbnailSnapshot.status == files::DirectoryStatus::ERROR))) {
        m_thumbnailRevision = thumbnailSnapshot.generation;
        m_thumbnailContext = m_browser.contextSerial();
        m_thumbnails.invalidate();
      }
      if (std::exchange(m_settingsDirty, false)) {
        if (m_pendingSettings)
          applySettings(m_pendingSettings->browser, !m_preferenceError.empty());
        if (m_settingsView) {
          const auto& inherited = m_preferences.inheritedSettings();
          m_settingsView->sync(
              m_savedSettings, inherited ? inherited->browser : files::BrowserSettings{},
              m_preferences.busy() || m_thumbnails.clearing(),
              m_preferenceError.empty() ? m_thumbnails.error() : m_preferenceError, m_thumbnails.clearing()
          );
          m_surface.requestLayout();
        }
      }
      m_sidebarResizeLine->setFill(colorSpecFromRole(
          m_resizingSidebar || m_sidebarResize->focused() ? ColorRole::Primary
              : m_sidebarResize->hovered()                ? ColorRole::OnSurfaceVariant
                                                          : ColorRole::OutlineVariant
      ));
      syncTransferConflict();
      if (std::exchange(m_trashDirty, false))
        syncTrash();
      if (std::exchange(m_emptyTrashDirty, false))
        syncEmptyTrash();
      if (std::exchange(m_creationDirty, false))
        syncCreation();
      if (std::exchange(m_renameDirty, false))
        syncFileRename();
      if (m_browserDirty) {
        m_browserDirty = false;
        syncPlaceRename();
        syncBrowser();
        layout = true;
      }
      if (const auto target = std::exchange(m_breadcrumbEditTarget, std::nullopt);
          target && target == m_browser.model().snapshot().location) {
        beginLocationEdit();
        layout = true;
      }
      if (layout) {
        UiPhaseScope phase(UiPhase::Layout);
        std::optional<InputDispatcher::TabFocusSnapshot> placesFocus;
        if (m_placesDirty) {
          for (const Node* node = m_dispatcher.focusedArea(); node != nullptr; node = node->parent()) {
            if (node == m_places) {
              placesFocus = m_dispatcher.captureTabFocus();
              break;
            }
          }
        }
        if (m_placesDirty) {
          m_placesDirty = false;
          m_places->invalidate();
        }
        if (m_drag && m_dragScrollDelta != 0) {
          const auto delta = std::exchange(m_dragScrollDelta, 0.0F);
          if (m_dragScrollInPlaces) {
            m_places->dragScroll(delta);
          } else {
            m_list->scrollDuringDrag(delta);
          }
        }
        const float searchWidth = m_surface.width() < 800 ? 148.0F : 220.0F;
        m_places->setVisible(m_showSidebar);
        m_places->setParticipatesInLayout(m_showSidebar);
        m_sidebarResize->setVisible(m_showSidebar);
        m_sidebarResize->setParticipatesInLayout(m_showSidebar);
        const float sidebarWidth = static_cast<float>(std::min(m_sidebarWidth, sidebarMaximumWidth()));
        m_places->setMinWidth(sidebarWidth);
        m_places->setMaxWidth(sidebarWidth);
        m_searchArea->setMinWidth(searchWidth);
        m_searchArea->setMaxWidth(searchWidth);
        if (m_breadcrumbsDirty) {
          m_breadcrumbsDirty = false;
          rebuildBreadcrumbs();
        }
        if (m_appliedViewMode != m_viewMode) {
          applyViewMode();
        }
        if (m_viewAnchor) {
          const auto& rows = m_browser.model().rows();
          const auto found = std::ranges::find(rows, *m_viewAnchor);
          if (found != rows.end()) {
            m_list->scrollToIndex(static_cast<std::size_t>(std::distance(rows.begin(), found)), true);
          }
          m_viewAnchor.reset();
        }
        if (m_pendingHistoryViewport) {
          const auto viewport = *m_pendingHistoryViewport;
          m_pendingHistoryViewport.reset();
          m_detailsOffset = viewport.horizontal;
          const auto& rows = m_browser.model().rows();
          const auto found = viewport.first ? std::ranges::find(rows, *viewport.first) : rows.end();
          if (found != rows.end())
            m_list->scrollToIndex(static_cast<std::size_t>(found - rows.begin()), true, viewport.withinRow);
          else
            m_list->scrollView().requestScrollToOffset(viewport.vertical);
        }
        const bool clientHeader = !m_surface.serverDecorated() && !m_surface.fullscreen();
        const bool columnOrderChanged = m_columnOrder != m_appliedColumnOrder;
        if (columnOrderChanged) {
          for (const auto column : m_columnOrder) {
            auto* header = columnHeader(column);
            m_detailsHeaderRow->addChild(m_detailsHeaderRow->removeChild(header));
          }
          m_appliedColumnOrder = m_columnOrder;
        }
        m_titlebar->setVisible(clientHeader);
        m_titlebar->setParticipatesInLayout(clientHeader);
        m_root->arrange(
            renderer, LayoutRect{0, 0, static_cast<float>(m_surface.width()), static_cast<float>(m_surface.height())}
        );
        if (m_settingsView) {
          const float top = clientHeader ? m_titlebar->height() : 0.0F;
          m_settingsView->setCompact(m_surface.width() < 800);
          m_settingsView->arrange(
              renderer, {0, top, static_cast<float>(m_surface.width()), static_cast<float>(m_surface.height()) - top}
          );
        }
        layoutDetails(renderer);
        const bool columnHoverDirty = std::exchange(m_columnHoverDirty, false);
        if (columnOrderChanged || columnHoverDirty)
          m_dispatcher.syncPointerHover();
        layoutColumnDrag(renderer);
        m_sidebarResizeLine->arrange(renderer, {2.5F, 0, 1.0F, m_sidebarResize->height()});
        layoutDrag(renderer);
        layoutCompletions(renderer);
        m_thumbnails.endFrame();
        if (placesFocus) {
          m_dispatcher.restoreTabFocus(std::move(*placesFocus));
        }
        if (m_restorePlaceFocus && m_menuSurface == nullptr && !m_settingsView) {
          const auto location = *std::exchange(m_restorePlaceFocus, std::nullopt);
          if (m_showSidebar) {
            if (auto* area = m_places->focusArea(location)) {
              m_dispatcher.setFocus(area);
            }
          }
        }
        if (m_dispatcher.focusedArea() == m_list->focusArea()) {
          m_textInput.notifyClientStateChanged(this, TextInputChangeCause::Other);
        }
      }
    }

    void buildScene() {
      uiAssertNotRendering("Window::buildScene");
      m_appliedColumnOrder = files::kDefaultDetailsColumnOrder;
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
      auto breadcrumbScroll = ui::scrollView(
          {.out = &m_breadcrumbScroll,
           .orientation = ScrollOrientation::Horizontal,
           .scrollbarVisible = false,
           .viewportPaddingH = 0.0F,
           .viewportPaddingV = 0.0F,
           .fillWidth = true,
           .fillHeight = true,
           .flexGrow = 1.0F}
      );
      breadcrumbScroll->setStickToBottom(true);
      breadcrumbScroll->content()->setDirection(FlexDirection::Horizontal);
      breadcrumbScroll->content()->addChild(
          ui::row({.out = &m_breadcrumbs, .align = FlexAlign::Center, .gap = 0.0F, .height = Style::controlHeight})
      );
      auto locationArea = ui::column(
          {.out = &m_locationArea,
           .align = FlexAlign::Stretch,
           .fill = colorSpecFromRole(ColorRole::SurfaceContainer),
           .radius = Style::radiusMd,
           .border = colorSpecFromRole(ColorRole::OutlineVariant),
           .minWidth = 80.0F,
           .height = Style::controlHeight,
           .flexGrow = 1.0F},
          std::move(breadcrumbScroll),
          ui::input(
              {.out = &m_location,
               .placeholder = "Location",
               .frameVisible = false,
               .minLayoutWidth = 72.0F,
               .flexGrow = 1.0F,
               .visible = false,
               .participatesInLayout = false,
               .onChange = [this](const std::string& text) { requestCompletions(text); },
               .onSubmit =
                   [this](const std::string& text) {
                     m_locationInvalid = !m_browser.openText(text);
                     if (!m_locationInvalid) {
                       endLocationEdit(true);
                     }
                     m_browserDirty = true;
                     m_surface.requestUpdate();
                   },
               .onFocusLoss =
                   [this] {
                     if (m_editingLocation) {
                       endLocationEdit(false);
                     }
                   }}
          )
      );
      auto searchArea = ui::row(
          {.out = &m_searchArea,
           .align = FlexAlign::Center,
           .gap = Style::spaceXs,
           .paddingH = Style::spaceSm,
           .fill = colorSpecFromRole(ColorRole::SurfaceContainer),
           .radius = Style::radiusMd,
           .border = colorSpecFromRole(ColorRole::OutlineVariant),
           .minWidth = 220.0F,
           .maxWidth = 220.0F,
           .height = Style::controlHeight,
           .configure = [](Flex& area) { area.setPadding(0.0F, 0.0F, 0.0F, Style::spaceSm); }},
          ui::glyph(
              {.glyph = "search",
               .glyphSize = Style::baseGlyphSize,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
          ),
          ui::input(
              {.out = &m_filterInput,
               .placeholder = "Search files…",
               .controlHeight = Style::controlHeight,
               .horizontalPadding = 0.0F,
               .clearButtonEnabled = true,
               .frameVisible = false,
               .minLayoutWidth = 72.0F,
               .flexGrow = 1.0F,
               .onChange = [this](const std::string& text) { changeNameFilter(text); },
               .onSubmit =
                   [this](const std::string&) {
                     if (m_filterInput->composing())
                       return;
                     m_browser.submitSearchQuery();
                     if (!m_browser.model().rows().empty()) {
                       m_dispatcher.setFocus(m_list->focusArea());
                     }
                   },
               .configure =
                   [this](Input& input) {
                     input.setOnCompositionChange([this](bool composing) { m_browser.setSearchComposing(composing); });
                     input.setClearButtonWidth(24.0F);
                     input.inputArea()->setTooltip("Search names in this folder and all subfolders");
                     input.setOnFocusGain([this] {
                       m_filterOpen = true;
                       m_browserDirty = true;
                       m_surface.requestUpdate();
                     });
                     input.setOnFocusLoss([this] {
                       m_browserDirty = true;
                       m_surface.requestUpdate();
                     });
                   }}
          )
      );
      auto toolbar = ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm, .padding = Style::spaceSm},
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceXs},
              ui::button(
                  {.out = &m_back,
                   .glyph = "arrow-left",
                   .variant = ButtonVariant::Ghost,
                   .tooltip = "Back (Alt+Left)",
                   .onClick = [this] { dispatchAction(Action::BACK); }}
              ),
              ui::button(
                  {.out = &m_forward,
                   .glyph = "arrow-right",
                   .variant = ButtonVariant::Ghost,
                   .tooltip = "Forward (Alt+Right)",
                   .onClick = [this] { dispatchAction(Action::FORWARD); }}
              ),
              ui::button(
                  {.out = &m_up,
                   .glyph = "arrow-up",
                   .variant = ButtonVariant::Ghost,
                   .tooltip = "Parent folder (Alt+Up)",
                   .onClick = [this] { dispatchAction(Action::UP); }}
              )
          ),
          std::move(locationArea), std::move(searchArea),
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceXs},
              ui::button(
                  {.out = &m_refresh,
                   .glyph = "refresh",
                   .variant = ButtonVariant::Ghost,
                   .onClick =
                       [this] {
                         if (m_browser.model().snapshot().status == files::DirectoryStatus::LOADING) {
                           dispatchAction(Action::STOP);
                         } else {
                           dispatchAction(Action::REFRESH);
                         }
                       }}
              ),
              ui::segmented(
                  {.out = &m_viewSwitch,
                   .options =
                       std::vector<ui::SegmentedOption>{
                           {.label = "", .glyph = "layout-grid", .tooltip = "Grid view (Ctrl+1)"},
                           {.label = "", .glyph = "list", .tooltip = "List view (Ctrl+2)"}
                       },
                   .selectedIndex = m_viewMode == files::ViewMode::GRID ? 0U : 1U,
                   .compact = true,
                   .onChange = [this](std::size_t index) { dispatchAction(index == 0 ? Action::GRID : Action::LIST); }}
              ),
              ui::button(
                  {.out = &m_menuButton, .glyph = "dots-vertical", .variant = ButtonVariant::Ghost, .onClick = [this] {
                     openMenu();
                   }}
              )
          )
      );

      auto places = std::make_unique<files::PlacesSidebar>(m_pins, m_icons);
      m_places = places.get();
      places->setOpenCallback([this](const files::Location& location) {
        m_actionError.clear();
        m_browser.open(location);
        m_dispatcher.setFocus(m_list->focusArea());
      });
      places->setContextCallback([this](const files::Bookmark& bookmark, float x, float y) {
        openPlaceMenu(bookmark, x, y);
      });
      places->setDragCallback([this](const files::Bookmark& bookmark) { startPlaceDrag(bookmark); });
      places->setRenameCallback([this](const files::Bookmark& bookmark) { openPlaceRename(bookmark); });
      auto sidebarResize = ui::inputArea(
          {.out = &m_sidebarResize,
           .acceptedButtons = InputArea::buttonMask(BTN_LEFT),
           .cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE,
           .focusable = true,
           .tabStop = true,
           .tooltip = "Resize Places; Left/Right adjusts, Home resets",
           .width = kSidebarHandleWidth,
           .onEnter = [this](const auto&) { m_surface.requestUpdate(); },
           .onLeave = [this] { m_surface.requestUpdate(); },
           .onMotion =
               [this](const InputArea::PointerData& pointer) {
                 if (!m_resizingSidebar)
                   return;
                 float x = 0, y = 0;
                 Node::absolutePosition(m_sidebarResize, x, y);
                 const float delta = (x + pointer.localX - m_sidebarPressX) * (Style::rtl() ? -1.0F : 1.0F);
                 resizeSidebar(m_sidebarStartVisibleWidth + static_cast<int>(std::lround(delta)), false);
               },
           .onPress =
               [this](const InputArea::PointerData& pointer) {
                 if (pointer.pressed) {
                   m_sidebarStartWidth = m_sidebarWidth;
                   m_sidebarStartVisibleWidth = static_cast<int>(std::lround(m_places->width()));
                   m_sidebarPressX = pointer.sceneX;
                   m_resizingSidebar = true;
                 } else if (std::exchange(m_resizingSidebar, false) && m_sidebarWidth != m_sidebarStartWidth) {
                   m_preferences.saveSidebarWidth(m_sidebarWidth);
                 }
                 m_surface.requestUpdate();
               },
           .onKeyDown =
               [this](const InputArea::KeyData& key) {
                 if (!key.pressed || key.preedit || (key.modifiers != 0 && key.modifiers != KeyMod::Shift))
                   return;
                 const int step = key.modifiers == KeyMod::Shift ? 24 : 8;
                 if (KeySymbol::isLeft(key.sym) || KeySymbol::isRight(key.sym)) {
                   const int direction = KeySymbol::isRight(key.sym) != Style::rtl() ? 1 : -1;
                   resizeSidebar(static_cast<int>(std::lround(m_places->width())) + direction * step, true);
                 } else if (KeySymbol::isHome(key.sym) && key.modifiers == 0) {
                   resizeSidebar(files::kDefaultSidebarWidth, true);
                 }
               },
           .onFocusGain = [this] { m_surface.requestUpdate(); },
           .onFocusLoss = [this] { m_surface.requestUpdate(); },
           .configure =
               [this](InputArea& handle) {
                 handle.setTabFocusKey("places-width");
                 handle.setOnCancel([this] {
                   if (std::exchange(m_resizingSidebar, false)) {
                     m_sidebarWidth = m_sidebarStartWidth;
                     if (m_dispatcher.focusedArea() == m_sidebarResize)
                       m_dispatcher.setFocus(nullptr);
                     m_surface.requestLayout();
                   }
                 });
                 handle.addChild(ui::box({.out = &m_sidebarResizeLine}));
               }}
      );
      auto content = ui::column(
          {.out = &m_filePane,
           .align = FlexAlign::Stretch,
           .gap = Style::spaceXs,
           .padding = Style::spaceSm,
           .flexGrow = 1.0F},
          ui::column(
              {.out = &m_listHeader, .align = FlexAlign::Stretch},
              ui::row(
                  {.height = 32.0F},
                  ui::row(
                      {.out = &m_detailsHeaderViewport,
                       .align = FlexAlign::Stretch,
                       .clipChildren = true,
                       .height = 32.0F,
                       .flexGrow = 1.0F},
                      ui::row(
                          {.out = &m_detailsHeaderRow,
                           .align = FlexAlign::Center,
                           .paddingH = Style::spaceMd - Style::spaceXs,
                           .participatesInLayout = false},
                          ui::button(
                              {.out = &m_nameHeader,
                               .text = "Name",
                               .fontSize = Style::fontSizeCaption,
                               .controlHeight = Style::controlHeightSm,
                               .contentAlign = ButtonContentAlign::Start,
                               .variant = ButtonVariant::Ghost,
                               .tooltip = "Sort by name; click again to reverse",
                               .paddingH = Style::spaceXs,
                               .onClick = [this] { sortBy(files::SortField::NAME); }}
                          ),
                          ui::button(
                              {.out = &m_typeHeader,
                               .text = "Type",
                               .fontSize = Style::fontSizeCaption,
                               .controlHeight = Style::controlHeightSm,
                               .contentAlign = ButtonContentAlign::Start,
                               .variant = ButtonVariant::Ghost,
                               .tooltip = "Sort by type; click again to reverse",
                               .minWidth = 144.0F + 2 * Style::spaceXs,
                               .maxWidth = 144.0F + 2 * Style::spaceXs,
                               .paddingH = Style::spaceXs,
                               .onClick = [this] { sortBy(files::SortField::TYPE); }}
                          ),
                          ui::button(
                              {.out = &m_sizeHeader,
                               .text = "Size",
                               .fontSize = Style::fontSizeCaption,
                               .controlHeight = Style::controlHeightSm,
                               .contentAlign = ButtonContentAlign::Start,
                               .variant = ButtonVariant::Ghost,
                               .tooltip = "Sort by size; click again to reverse",
                               .minWidth = 110.0F + 2 * Style::spaceXs,
                               .maxWidth = 110.0F + 2 * Style::spaceXs,
                               .paddingH = Style::spaceXs,
                               .onClick = [this] { sortBy(files::SortField::SIZE); }}
                          ),
                          ui::button(
                              {.out = &m_modifiedHeader,
                               .text = "Modified",
                               .fontSize = Style::fontSizeCaption,
                               .controlHeight = Style::controlHeightSm,
                               .contentAlign = ButtonContentAlign::Start,
                               .variant = ButtonVariant::Ghost,
                               .tooltip = "Sort by modification time; click again to reverse",
                               .minWidth = 148.0F + 2 * Style::spaceXs,
                               .maxWidth = 148.0F + 2 * Style::spaceXs,
                               .paddingH = Style::spaceXs,
                               .onClick = [this] { sortBy(files::SortField::MODIFIED); }}
                          )
                      )
                  ),
                  ui::box({.width = Style::scrollbarWidth + Style::scrollbarGap})
              ),
              ui::separator({})
          ),
          ui::column(
              {.out = &m_noticeArea, .align = FlexAlign::Stretch, .flexGrow = 1.0F},
              ui::label({.out = &m_notice, .textAlign = TextAlign::Center})
          ),
          ui::virtualGridView(
              {.out = &m_list,
               .columns = 1,
               .cellHeight = 36.0F,
               .squareCells = false,
               .columnGap = 0.0F,
               .rowGap = 2.0F,
               .adapter = &m_listAdapter,
               .flexGrow = 1.0F,
               .configure = [this](VirtualGridView& list) {
                 list.scrollView().setReserveScrollbarGutter(true);
                 list.focusArea()->setRetainsFocusOnPointerRelease(true);
                 list.focusArea()->setTextInputClient(this);
                 list.focusArea()->setOnFocusGain([this] {
                   m_browserDirty = true;
                   m_surface.requestUpdate();
                 });
                 list.focusArea()->setOnFocusLoss([this] {
                   m_browserDirty = true;
                   m_surface.requestUpdate();
                 });
               }}
          )
      );
      auto horizontalBar = std::make_unique<Scrollbar>();
      horizontalBar->setOrientation(ScrollOrientation::Horizontal);
      horizontalBar->setParticipatesInLayout(false);
      horizontalBar->setOnScrollChanged([this](float offset) { scrollDetails(offset); });
      m_detailsScrollbar = horizontalBar.get();
      auto horizontalHost = ui::box(
          {.out = &m_detailsScrollHost,
           .height = Style::scrollbarWidth + Style::scrollbarGap,
           .visible = false,
           .participatesInLayout = false}
      );
      horizontalHost->addChild(std::move(horizontalBar));
      content->addChild(std::move(horizontalHost));
      m_root = ui::column(
          {.align = FlexAlign::Stretch,
           .fill = colorSpecFromRole(ColorRole::Surface),
           .fillWidth = true,
           .fillHeight = true},
          std::move(header), std::move(toolbar),
          ui::row(
              {.align = FlexAlign::Stretch, .clipChildren = true, .flexGrow = 1.0F}, std::move(places),
              std::move(sidebarResize), std::move(content)
          ),
          ui::row(
              {.align = FlexAlign::Center,
               .gap = Style::spaceMd,
               .padding = Style::spaceSm,
               .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow)},
              ui::label(
                  {.out = &m_status,
                   .maxLines = 1,
                   .flexGrow = 1.0F,
                   .configure = [](Label& label) { label.setUseMarkup(false); }}
              ),
              ui::label(
                  {.out = &m_freeSpace,
                   .fontSize = Style::fontSizeCaption,
                   .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                   .maxWidth = 160.0F,
                   .maxLines = 1,
                   .textAlign = TextAlign::End}
              ),
              ui::button(
                  {.out = &m_cancelOperation,
                   .glyph = "x",
                   .controlHeight = Style::controlHeightSm,
                   .variant = ButtonVariant::Ghost,
                   .tooltip = "Cancel transfer",
                   .visible = false,
                   .participatesInLayout = false,
                   .onClick = [this] {
                     m_transfers.cancel();
                     m_trash.cancel();
                     m_emptyTrash.cancel();
                   }}
              )
          )
      );
      for (std::size_t index = 0; index < 4; ++index) {
        auto handle = std::make_unique<InputArea>();
        handle->setParticipatesInLayout(false);
        handle->setZIndex(10);
        handle->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE);
        handle->setAcceptedButtons(InputArea::buttonMask(BTN_LEFT));
        handle->setOnPress([this, index](const InputArea::PointerData& data) {
          if (data.pressed) {
            captureViewAnchor();
            m_list->cancelPointerGesture();
            m_columnResize = ColumnResize{
                index,          data.sceneX,     headerWidth(index),
                m_columnWidths, m_detailsOffset, static_cast<int>(std::lround(m_detailsColumns.nameWidth))
            };
          } else if (m_columnResize) {
            const auto before = m_columnResize->before;
            m_columnResize.reset();
            if (m_columnWidths != before)
              m_preferences.saveColumnWidths(m_columnWidths);
            m_columnHoverDirty = true;
          }
          m_surface.requestLayout();
        });
        handle->setOnMotion([this](const InputArea::PointerData&) {
          if (!m_columnResize)
            return;
          const auto& resize = *m_columnResize;
          m_columnWidths = resize.before;
          const int allocation = std::clamp(
              static_cast<int>(std::lround(resize.width + (m_pointerX - resize.pressX) * (Style::rtl() ? -1 : 1))),
              files::minimumDetailsColumnWidth(resize.index), files::maximumDetailsColumnWidth(resize.index)
          );
          if (allocation != static_cast<int>(std::lround(resize.width))) {
            m_columnWidths[resize.index] = allocation;
            if (resize.index != 0 && m_columnWidths[0] == 0)
              m_columnWidths[0] = resize.nameWidth;
          }
          m_surface.requestLayout();
        });
        handle->setOnCancel([this] { cancelColumnResize(); });
        handle->setOnEnter([this](const auto&) { m_surface.requestLayout(); });
        handle->setOnLeave([this] { m_surface.requestLayout(); });
        handle->addChild(
            ui::box(
                {.out = &m_columnResizeLines[index],
                 .fill = colorSpecFromRole(ColorRole::Primary),
                 .participatesInLayout = false,
                 .configure = [](Box& line) { line.setHitTestVisible(false); }}
            )
        );
        m_columnResizeHandles[index] = static_cast<InputArea*>(m_detailsHeaderViewport->addChild(std::move(handle)));
      }
      for (const auto column : m_columnOrder) {
        auto* header = columnHeader(column);
        header->setOnPress([this, column, header](float, float, bool pressed) {
          if (pressed) {
            m_columnDrag = ColumnDrag{column, m_pointerX, m_pointerY, header->inputArea()->cursorShape()};
          } else {
            cancelColumnDrag();
          }
        });
        header->setOnMotion([this] { updateColumnDrag(); });
        header->inputArea()->setOnCancel([this] { cancelColumnDrag(); });
      }
      m_root->addChild(
          ui::box(
              {.out = &m_columnDropLine,
               .fill = colorSpecFromRole(ColorRole::Primary),
               .visible = false,
               .participatesInLayout = false}
          )
      );
      m_columnDropLine->setZIndex(102);
      m_columnDropLine->setHitTestVisible(false);
      auto dragIcon = std::make_unique<files::FileIcon>(m_icons, 40);
      m_dragIcon = dragIcon.get();
      m_dragIcon->setVisible(false);
      m_dragIcon->setParticipatesInLayout(false);
      m_dragIcon->setOpacity(0.85F);
      m_dragIcon->setZIndex(100);
      m_root->addChild(std::move(dragIcon));
      m_root->addChild(
          ui::column(
              {.out = &m_dragCount,
               .align = FlexAlign::Center,
               .justify = FlexJustify::Center,
               .fill = colorSpecFromRole(ColorRole::Primary),
               .radius = Style::radiusSm,
               .visible = false,
               .participatesInLayout = false},
              ui::label(
                  {.out = &m_dragCountLabel,
                   .fontSize = Style::fontSizeCaption,
                   .color = colorSpecFromRole(ColorRole::OnPrimary)}
              )
          )
      );
      m_dragCount->setHitTestVisible(false);
      m_dragCount->setZIndex(101);
      m_root->addChild(
          ui::box(
              {.out = &m_placeDropLine,
               .fill = colorSpecFromRole(ColorRole::Primary),
               .height = 2.0F,
               .visible = false,
               .participatesInLayout = false}
          )
      );
      m_placeDropLine->setZIndex(98);
      m_placeDropLine->setHitTestVisible(false);
      m_root->addChild(
          ui::box(
              {.out = &m_folderDropHighlight,
               .fill = colorSpecFromRole(ColorRole::Primary, 0.16F),
               .border = colorSpecFromRole(ColorRole::Primary),
               .borderWidth = Style::focusRingWidth,
               .radius = Style::radiusSm,
               .visible = false,
               .participatesInLayout = false}
          )
      );
      m_folderDropHighlight->setZIndex(99);
      m_folderDropHighlight->setHitTestVisible(false);
      auto completion = std::make_unique<ContextMenuControl>();
      m_completionControl = completion.get();
      completion->setOnActivate([this](const ContextMenuControlEntry& entry) {
        acceptCompletion(static_cast<std::size_t>(entry.id), false);
      });
      auto completionFrame = ui::column(
          {.out = &m_completionFrame,
           .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
           .radius = Style::radiusLg,
           .border = colorSpecFromRole(ColorRole::OutlineVariant),
           .visible = false,
           .participatesInLayout = false},
          std::move(completion)
      );
      completionFrame->setZIndex(110);
      m_root->addChild(std::move(completionFrame));
      m_root->setAnimationManager(&m_animations);
      m_surface.setSceneRoot(m_root.get());
      m_dispatcher.setSceneRoot(m_root.get());
      m_dispatcher.setTextInputContext(m_surface.wlSurface(), &m_textInput);
      m_dispatcher.setFocus(m_list->focusArea());
    }

    void revealMenuHighlight() {
      if (!m_menuScroll || !m_menu)
        return;
      const auto index = m_menu->highlightedIndex();
      const float top = m_menu->rowTop(index), bottom = m_menu->rowBottom(index);
      const float offset = m_menuScroll->scrollOffset();
      if (top < offset)
        m_menuScroll->requestScrollToOffset(top);
      else if (bottom > offset + m_menuScroll->height())
        m_menuScroll->requestScrollToOffset(bottom - m_menuScroll->height());
      m_menuSurface->requestLayout();
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
      if (std::exchange(m_emptyTrashDialog, false))
        m_emptyTrash.cancel();
      m_emptyTrashMessage = nullptr;
      m_emptyTrashSize = nullptr;
      m_emptyTrashConfirm = nullptr;
      if (const auto id = std::exchange(m_activeTransferConflict, std::nullopt)) {
        (void)m_transfers.resolve(*id, files::TransferConflictChoice::CANCEL);
      }
      if (m_placeRenameTarget) {
        m_restorePlaceFocus = m_placeRenameTarget->location;
      }
      if (m_fileRenameTarget) {
        m_renamer.cancel();
        if (m_list != nullptr)
          m_dispatcher.setFocus(m_list->focusArea());
      }
      if (m_createParent) {
        m_creator.cancel();
        if (m_list != nullptr)
          m_dispatcher.setFocus(m_list->focusArea());
      }
      m_createEditor = nullptr;
      m_createParent.reset();
      m_creationPreparing = false;
      m_fileRenameEditor = nullptr;
      m_fileRenameTarget.reset();
      m_fileRenamePreparing = false;
      m_placeEditor = nullptr;
      m_placeRenameTarget.reset();
      m_pendingPlaceLabel.reset();
      m_menuDispatcher.setSceneRoot(nullptr);
      m_menuDispatcher.setTextInputContext(nullptr, nullptr);
      if (m_menuSurface != nullptr) {
        m_menuSurface->setSceneRoot(nullptr);
      }
      m_patternEditor = nullptr;
      if (auto dismiss = std::exchange(m_selectDismiss, {}))
        dismiss();
      m_aboutScroll = nullptr;
      m_aboutMessage = nullptr;
      m_dialogHeight.reset();
      m_dialogPosition.reset();
      m_menuDispatcher.setFocusChangeCallback({});
      m_menuRoot.reset();
      m_menu = nullptr;
      m_menuScroll = nullptr;
      m_menuSurface.reset();
      m_dispatcher.setTextInputContext(m_surface.wlSurface(), &m_textInput);
      m_surface.requestLayout();
    }
    static LayoutRect sceneRect(const Node& node) {
      float x = 0, y = 0;
      Node::absolutePosition(&node, x, y);
      return {x, y, node.width(), node.height()};
    }

    void showPopup(
        std::unique_ptr<Flex> root, ContextMenuControl* menu, LayoutRect anchor, float width, float height,
        bool alignLeft = false
    ) {
      clearTypeahead();
      clearCompletions();
      closeMenu();
      m_menu = menu;
      m_menuRoot = std::move(root);
      m_menuSurface = std::make_unique<PopupSurface>(m_connection);
      m_menuSurface->setRenderContext(&m_renderer);
      m_menuSurface->setAnimationManager(&m_animations);
      m_menuSurface->setDismissedCallback([this] { dismissMenu(); });
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
        if (m_dialogHeight) {
          const auto width =
              static_cast<std::uint32_t>(std::min(588.0F, static_cast<float>(m_surface.width()) - 32.0F));
          LayoutConstraints constraints;
          constraints.setExactWidth(static_cast<float>(width));
          m_dialogHeight = m_menuRoot->measure(m_menuSurface->renderTarget().renderer(), constraints).height;
          const auto height = static_cast<std::uint32_t>(
              std::ceil(std::min(*m_dialogHeight, static_cast<float>(m_surface.height()) - 64.0F))
          );
          if (width != m_menuSurface->width() || height != m_menuSurface->height())
            (void)m_menuSurface->resize(width, height, false);
          const std::pair<int, int> position{
              static_cast<int>((m_surface.width() - width) / 2), static_cast<int>((m_surface.height() - height) / 2)
          };
          if (m_dialogPosition != position) {
            m_dialogPosition = position;
            (void)m_menuSurface->repositionAnchor(
                {.anchorX = position.first,
                 .anchorY = position.second,
                 .anchorWidth = 1,
                 .anchorHeight = 1,
                 .anchor = XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT,
                 .gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
                 .constraintAdjustment =
                     XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y},
                false
            );
          }
        }
        if (m_emptyTrashDialog) {
          LayoutConstraints constraints;
          constraints.setExactWidth(static_cast<float>(m_menuSurface->width()));
          const auto height = static_cast<std::uint32_t>(std::ceil(
              std::clamp(
                  m_menuRoot->measure(m_menuSurface->renderTarget().renderer(), constraints).height, 1.0F,
                  static_cast<float>(m_surface.height()) - 64.0F
              )
          ));
          if (height != m_menuSurface->height()) {
            const auto width = m_menuSurface->width();
            (void)m_menuSurface->resize(width, height, false);
            (void)m_menuSurface->repositionAnchor(
                {.anchorX = static_cast<int>((m_surface.width() - width) / 2),
                 .anchorY = static_cast<int>((m_surface.height() - height) / 2),
                 .anchorWidth = 1,
                 .anchorHeight = 1,
                 .anchor = XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT,
                 .gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
                 .constraintAdjustment =
                     XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y},
                false
            );
          }
        }
        m_menuRoot->arrange(
            m_menuSurface->renderTarget().renderer(),
            {0, 0, static_cast<float>(m_menuSurface->width()), static_cast<float>(m_menuSurface->height())}
        );
      });
      m_menuSurface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_menuSurface->requestLayout(); });
      PopupSurfaceConfig config{
          .anchorX = static_cast<int>(anchor.x),
          .anchorY = static_cast<int>(anchor.y),
          .anchorWidth = std::max(1, static_cast<int>(anchor.width)),
          .anchorHeight = std::max(1, static_cast<int>(anchor.height)),
          .width = static_cast<std::uint32_t>(width),
          .height = static_cast<std::uint32_t>(height),
          .anchor = XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT,
          .gravity = alignLeft ? XDG_POSITIONER_GRAVITY_BOTTOM_LEFT : XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
          .constraintAdjustment =
              XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y,
          .serial = m_popupSerial != 0 ? m_popupSerial : m_connection.input().lastSerial(),
      };
      if (!m_menuSurface->initializeAsChild(
              m_surface.xdgSurface(), m_connection.outputForSurface(m_surface.wlSurface()), config
          )) {
        closeMenu();
      } else {
        m_menuDispatcher.setTextInputContext(m_menuSurface->wlSurface(), &m_textInput, true, m_surface.wlSurface());
      }
    }

    void showContextMenu(
        std::vector<ContextMenuControlEntry> entries, std::function<void(int)> action, LayoutRect anchor,
        bool alignLeft = false, float preferredWidth = 264.0F
    ) {
      if (m_columnDrag || m_columnResize)
        m_dispatcher.cancelPointerCapture();
      cancelDrag();
      auto menu = std::make_unique<ContextMenuControl>();
      auto* control = menu.get();
      const float contentHeight = ContextMenuControl::preferredHeight(entries, entries.size(), 1.0F);
      const float height = std::min(contentHeight, std::max(120.0F, static_cast<float>(m_surface.height()) - 32.0F));
      control->setMaxVisible(entries.size());
      const float width = std::min(preferredWidth, static_cast<float>(m_surface.width()) - 32.0F);
      control->setEntries(std::move(entries));
      const bool overflowing = contentHeight > height + 0.5F;
      const float gutter = overflowing ? Style::scrollbarWidth + Style::scrollbarGap : 0.0F;
      // The menu has an intrinsic width; reserve the scroller's lane explicitly.
      control->setMenuWidth(width - gutter);
      control->setOnActivate([this, action = std::move(action)](const ContextMenuControlEntry& entry) {
        const int id = entry.id;
        const std::weak_ptr<int> lifetime = m_lifetime;
        dismissMenu();
        DeferredCall::callLater([lifetime, action, id] {
          if (!lifetime.expired()) {
            action(id);
          }
        });
      });
      ScrollView* scroll = nullptr;
      auto scrolling = ui::scrollView(
          {.out = &scroll, .viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F, .height = height, .flexGrow = 1.0F}
      );
      scrolling->setReserveScrollbarGutter(overflowing);
      scrolling->content()->addChild(std::move(menu));
      auto root = ui::column(
          {.fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh), .radius = Style::radiusLg}, std::move(scrolling)
      );
      showPopup(std::move(root), control, anchor, width, height, alignLeft);
      m_menuScroll = scroll;
    }

    void openColumnsMenu(const LayoutRect& anchor, std::optional<files::DetailsColumn> column = std::nullopt) {
      std::vector<ContextMenuControlEntry> entries{
          {.label = "Name", .enabled = false, .checkmark = true, .toggleState = 1},
      };
      for (const auto candidate : m_columnOrder) {
        const char* label = candidate == files::DetailsColumn::TYPE ? "Type"
            : candidate == files::DetailsColumn::SIZE               ? "Size"
                                                                    : "Modified";
        entries.push_back(
            {.id = static_cast<int>(candidate) + 1,
             .label = label,
             .checkmark = true,
             .toggleState = files::detailsColumnVisible(m_columnVisibility, candidate) ? 1 : 0}
        );
      }
      entries.push_back({.label = "", .separator = true});
      entries.push_back(
          {.id = 4, .label = "Show all columns", .enabled = m_columnVisibility != files::DetailsColumnVisibility{}}
      );
      const int left = Style::rtl() ? 1 : -1;
      if (column) {
        entries.push_back({.label = "", .separator = true});
        entries.push_back({.id = 5, .label = "Move left", .enabled = movedColumnOrder(*column, left).has_value()});
        entries.push_back({.id = 6, .label = "Move right", .enabled = movedColumnOrder(*column, -left).has_value()});
      }
      entries.push_back({.label = "", .separator = true});
      entries.push_back(
          {.id = 7, .label = "Reset column order", .enabled = m_columnOrder != files::kDefaultDetailsColumnOrder}
      );
      const std::size_t widthIndex = column ? static_cast<std::size_t>(*column) + 1 : 0;
      entries.push_back(
          {.id = 8,
           .label = "Reset column width",
           .enabled = m_columnWidths[widthIndex] != files::kDefaultDetailsColumnWidths[widthIndex]}
      );
      entries.push_back(
          {.id = 9, .label = "Reset all column widths", .enabled = m_columnWidths != files::kDefaultDetailsColumnWidths}
      );
      showContextMenu(
          std::move(entries),
          [this, column, left, widthIndex](int id) {
            if (id == 8 || id == 9) {
              auto widths = id == 9 ? files::kDefaultDetailsColumnWidths : m_columnWidths;
              widths[widthIndex] = files::kDefaultDetailsColumnWidths[widthIndex];
              setDetailsColumnWidths(widths);
              m_dispatcher.setFocus(m_list->focusArea());
              return;
            }
            if ((id == 5 || id == 6) && column) {
              if (const auto order = movedColumnOrder(*column, id == 5 ? left : -left))
                setDetailsColumnOrder(*order);
              m_dispatcher.setFocus(m_list->focusArea());
              return;
            }
            if (id == 7) {
              setDetailsColumnOrder(files::kDefaultDetailsColumnOrder);
              m_dispatcher.setFocus(m_list->focusArea());
              return;
            }
            auto visibility = m_columnVisibility;
            if (id == 1)
              visibility.type = !visibility.type;
            else if (id == 2)
              visibility.size = !visibility.size;
            else if (id == 3)
              visibility.modified = !visibility.modified;
            else if (id == 4)
              visibility = {};
            setDetailsColumnVisibility(visibility);
            m_dispatcher.setFocus(m_list->focusArea());
          },
          anchor
      );
    }

    void toggleHidden() {
      m_browser.toggleHidden();
      m_preferences.saveShowHidden(m_browser.model().filter().showHidden);
    }

    void showActionMenu(std::initializer_list<Action> actions, bool radio = false, bool back = false) {
      const auto request = actionRequest(false);
      std::vector<ContextMenuControlEntry> entries;
      if (back) {
        entries.push_back(actionEntry(Action::VIEW_MENU, request));
        entries.push_back({.label = "", .separator = true});
      }
      for (const auto action : actions)
        entries.push_back(actionEntry(action, request, radio && action != Action::FOLDERS_FIRST));
      showContextMenu(
          std::move(entries),
          [this, request](int id) {
            const auto action = static_cast<Action>(id);
            dispatchAction(action, request);
            if (!m_menuSurface && action != Action::SEARCH && action != Action::EDIT_LOCATION)
              m_dispatcher.setFocus(m_list->focusArea());
          },
          sceneRect(*m_menuButton), true
      );
    }
    void openSortMenu() {
      showActionMenu(
          {Action::SORT_NAME, Action::SORT_TYPE, Action::SORT_SIZE, Action::SORT_MODIFIED, Action::ASCENDING,
           Action::DESCENDING, Action::FOLDERS_FIRST},
          true, true
      );
    }
    void openSelectionMenu() {
      showActionMenu(
          {Action::SELECT_ALL, Action::INVERT_SELECTION, Action::SELECT_PATTERN, Action::CLEAR_SELECTION}, false, true
      );
    }
    void openInteractionMenu() {
      const auto request = actionRequest(false);
      std::vector<ContextMenuControlEntry> entries{
          actionEntry(Action::VIEW_MENU, request),
          {.label = "Open items with", .header = true},
          actionEntry(Action::SINGLE_CLICK, request, true),
          actionEntry(Action::DOUBLE_CLICK, request, true),
          {.label = "Typing in the file view", .header = true},
          actionEntry(Action::TYPE_TO_SEARCH, request, true),
          actionEntry(Action::TYPEAHEAD, request, true)
      };
      showContextMenu(
          std::move(entries), [this, request](int id) { dispatchAction(static_cast<Action>(id), request); },
          sceneRect(*m_menuButton), true
      );
    }
    void openMenu() {
      if (m_menuSurface != nullptr) {
        dismissMenu();
        return;
      }
      showActionMenu(
          {Action::SORT_MENU, Action::SELECTION_MENU, Action::INTERACTION_MENU, Action::SHOW_HIDDEN,
           Action::SHOW_SIDEBAR, Action::SEARCH, Action::SETTINGS, Action::ABOUT}
      );
    }

    WaylandConnection& m_connection;
    RenderContext& m_renderer;
    TextInputService& m_textInput;
    AnimationManager m_animations;
    ToplevelSurface m_surface;
    files::BrowserController m_browser;
    files::ActivationMode m_activationMode = files::ActivationMode::DOUBLE_CLICK;
    files::TypingMode m_typingMode = files::TypingMode::SEARCH;
    files::TypeaheadSelection m_typeahead;
    std::string m_typeaheadPreedit;
    Timer m_typeaheadTimer;
    std::uint64_t m_typeaheadContext = 0;
    NameEditor* m_patternEditor = nullptr;
    std::string m_selectionPattern = "*";
    files::FileIconCache m_icons;
    files::ThumbnailService m_thumbnails;
    std::uint64_t m_thumbnailRevision = 0;
    std::uint64_t m_thumbnailContext = 0;
    files::FileViewAdapter m_listAdapter;
    files::FileViewAdapter m_gridAdapter;
    files::ViewPreferenceWriter m_preferences;
    files::BrowserSettings m_savedSettings;
    std::optional<files::Settings> m_pendingSettings;
    bool m_settingsDirty = false;
    BrowserSettingsView* m_settingsView = nullptr;
    std::shared_ptr<int> m_settingsLifetime;
    std::function<void()> m_selectDismiss;
    ScrollView* m_aboutScroll = nullptr;
    Label* m_aboutMessage = nullptr;
    std::optional<float> m_dialogHeight;
    std::optional<std::pair<int, int>> m_dialogPosition;
    files::PinService m_pins;
    files::FileLauncher m_launcher;
    files::PlacesSidebar* m_places = nullptr;
    bool m_placesDirty = true;
    std::string m_actionError;
    std::optional<files::Location> m_revealTarget;
    std::optional<Drag> m_drag;
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
    files::TransferService m_transfers;
    files::TrashService m_trash;
    files::EmptyTrashService m_emptyTrash;
    bool m_emptyTrashDirty = false;
    bool m_emptyTrashDialog = false;
    Label* m_emptyTrashMessage = nullptr;
    Label* m_emptyTrashSize = nullptr;
    Button* m_emptyTrashConfirm = nullptr;
    bool m_trashDirty = false;
    files::RenameService m_renamer;
    files::CreateService m_creator;
    Timer m_operationProgress;
    Button* m_cancelOperation = nullptr;
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
    std::unique_ptr<Node> m_root;
    InputDispatcher m_dispatcher;
    Flex* m_titlebar = nullptr;
    Input* m_location = nullptr;
    Input* m_filterInput = nullptr;
    Flex* m_searchArea = nullptr;
    Flex* m_locationArea = nullptr;
    ScrollView* m_breadcrumbScroll = nullptr;
    Flex* m_breadcrumbs = nullptr;
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
    std::array<Box*, 4> m_columnResizeLines{};
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
    Button* m_refresh = nullptr;
    std::string m_displayedLocation;
    bool m_browserDirty = true;
    bool m_fileKeyboardFocus = false;
    bool m_locationInvalid = false;
    std::unique_ptr<PopupSurface> m_menuSurface;
    std::unique_ptr<Flex> m_menuRoot;
    ContextMenuControl* m_menu = nullptr;
    ScrollView* m_menuScroll = nullptr;
    NameEditor* m_placeEditor = nullptr;
    NameEditor* m_fileRenameEditor = nullptr;
    NameEditor* m_createEditor = nullptr;
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
} // namespace

int files::runApplication(const std::optional<std::string>& initialLocation) {
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
    GlibPollSource glib;
    TextInputService textInput;
    WaylandConnection connection;
    std::string error;
    if (!connection.initialize(error)) {
      throw std::runtime_error(error);
    }
    connection.bindTextInput(textInput);
    WaylandTextClipboard clipboard(connection.dataDeviceManager(), connection.seat(), [&connection] {
      return connection.input().lastSerial();
    });
    Input::setTextClipboard(&clipboard);
    struct ClipboardBinding {
      ~ClipboardBinding() { Input::setTextClipboard(nullptr); }
    } clipboardBinding;
    GlSharedContext graphics;
    graphics.initialize(connection.display());
    RenderContext renderer;
    renderer.initialize(graphics);
    Window window(
        connection, renderer, textInput, config.settings().browser, ConfigPaths::fromEnvironment(), clipboard
    );
    if (!window.initialize(initialLocation)) {
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
    IconThemePollSource icons;
    icons.setChangeCallback([&window] { window.iconThemeChanged(); });
    MainLoop loop(connection, {&deferred, &timers, &repeat, &glib, &icons}, [&window] {
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
