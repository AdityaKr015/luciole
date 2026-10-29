#include "app/application.h"

#include "app/deferred_call_poll_source.h"
#include "app/glib_poll_source.h"
#include "app/main_loop.h"
#include "app/timer_poll_source.h"
#include "browser/browser_controller.h"
#include "browser/file_view.h"
#include "browser/location_presentation.h"
#include "browser/place_label_editor.h"
#include "browser/places_sidebar.h"
#include "config/settings.h"
#include "config/view_preference_writer.h"
#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/timer_manager.h"
#include "core/ui_phase.h"
#include "platform/file_launcher.h"
#include "render/animation/animation_manager.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "system/icon_theme_poll_source.h"
#include "ui/builders.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/input.h"
#include "ui/controls/scroll_view.h"
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

  class Window : public TextInputClient {
  public:
    Window(
        WaylandConnection& connection, RenderContext& renderer, TextInputService& textInput,
        const files::BrowserSettings& settings, files::ConfigPaths paths
    )
        : m_connection(connection), m_renderer(renderer), m_textInput(textInput), m_surface(connection),
          m_browser(settings.sort, settings.showHidden),
          m_listAdapter(
              m_browser, m_icons, files::ViewMode::LIST, [this] { return m_connection.input().keyboardModifiers(); },
              [this] { return m_list != nullptr && m_list->focusArea()->focused(); }
          ),
          m_gridAdapter(
              m_browser, m_icons, files::ViewMode::GRID, [this] { return m_connection.input().keyboardModifiers(); },
              [this] { return m_list != nullptr && m_list->focusArea()->focused(); }
          ),
          m_preferences(std::move(paths)), m_pins(files::StateStore::pathFromEnvironment()),
          m_viewMode(settings.viewMode) {
      m_gridZoom = static_cast<std::size_t>(
          std::ranges::find(files::kGridIconSizes, settings.gridIconSize) - files::kGridIconSizes.begin()
      );
      m_surface.setRenderContext(&renderer);
      m_surface.setAnimationManager(&m_animations);
      m_surface.setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface.requestLayout(); });
      m_surface.setPrepareFrameCallback([this](bool update, bool layout) { prepareFrame(update, layout); });
      m_surface.setUpdateCallback([] {});
      m_surface.setDebugName("files-window");
      m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
        m_connection.input().setCursorShape(serial, shape);
      });
      m_browser.setChangedCallback([this] {
        m_browserDirty = true;
        m_surface.requestUpdate();
      });
      m_browser.setFileActivatedCallback([this](const files::Location& location) { m_launcher.open(location); });
      m_launcher.setChangedCallback([this](const std::string& error) {
        m_actionError = error.empty() ? "" : std::format("Could not open the file: {}", error);
        m_browserDirty = true;
        m_surface.requestUpdate();
      });
      m_pins.setChangedCallback([this] {
        m_placesDirty = true;
        m_browserDirty = true;
        m_surface.requestUpdate();
      });
      for (auto* adapter : {&m_listAdapter, &m_gridAdapter}) {
        adapter->setContextCallback([this](std::optional<files::EntryId> id, float x, float y) {
          openFileMenu(id, x, y);
        });
        adapter->setDragCallback(
            [this](const std::vector<files::EntryId>& ids) { startFilePinDrag(ids); },
            [this] {
              if (m_pinDrag && !m_pinDrag->sourcePin) {
                cancelPinDrag();
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
        m_preferenceError = error.empty() ? "" : std::format("Could not save the view preference: {}", error);
        m_browserDirty = true;
        m_surface.requestUpdate();
      });
    }

    ~Window() override {
      m_pinDragScroll.stop();
      m_browser.setChangedCallback({});
      m_browser.setFileActivatedCallback({});
      m_launcher.setChangedCallback({});
      m_pins.setChangedCallback({});
      m_icons.setChangedCallback({});
      m_preferences.setChangedCallback({});
      closeMenu();
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

    [[nodiscard]] bool running() const {
      return m_surface.isRunning() || m_preferences.busy() || m_pins.busy() || m_launcher.busy();
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
      m_pointerX = x;
      m_pointerY = y;
      if (event.type == PointerEvent::Type::Button && event.pressed) {
        m_popupSerial = event.serial;
      }
      if (dispatcher == &m_dispatcher
          && m_pinDrag
          && event.type == PointerEvent::Type::Button
          && event.button == BTN_LEFT
          && !event.pressed) {
        finishPinDrag(x, y);
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
          cancelPinDrag();
        }
        break;
      case PointerEvent::Type::Motion:
        dispatcher->pointerMotion(x, y, event.serial);
        if (dispatcher == &m_dispatcher && m_pinDrag) {
          updatePinDrag();
        }
        break;
      case PointerEvent::Type::Button:
        dispatcher->pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
        break;
      case PointerEvent::Type::Axis:
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
        if (m_pinDrag && KeySymbol::isEscape(event.sym)) {
          cancelPinDrag();
          return;
        }
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
          beginLocationEdit();
          return;
        }
        if ((event.sym == XKB_KEY_k || event.sym == XKB_KEY_f)
            && event.modifiers == KeyMod::Ctrl
            && m_filterInput != nullptr) {
          openFilter();
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
        if (event.modifiers == KeyMod::Alt && event.sym == XKB_KEY_Left) {
          (void)m_browser.back();
          return;
        }
        if (event.modifiers == KeyMod::Alt && event.sym == XKB_KEY_Right) {
          (void)m_browser.forward();
          return;
        }
        if (event.modifiers == KeyMod::Alt && event.sym == XKB_KEY_Up) {
          (void)m_browser.up();
          return;
        }
        if (event.sym == XKB_KEY_F5 && event.modifiers == 0) {
          m_browser.refresh();
          return;
        }
        const bool editing = (m_location != nullptr && m_dispatcher.focusedArea() == m_location->inputArea())
            || (m_filterInput != nullptr && m_dispatcher.focusedArea() == m_filterInput->inputArea());
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
              (void)m_list->absoluteAnchorForIndex(static_cast<std::size_t>(found - rows.begin()), x, y);
            }
          }
          openFileMenu(id, x, y);
          return;
        }
        if (!editing && event.sym == XKB_KEY_Return && event.modifiers == KeyMod::Alt) {
          std::vector<files::DirectoryEntry> targets;
          for (const auto id : m_browser.model().selectedIds()) {
            if (const auto* entry = m_browser.model().entry(id)) {
              targets.push_back(*entry);
            }
          }
          if (!targets.empty()) {
            showProperties(targets);
          }
          return;
        }
        if (!editing && event.modifiers == KeyMod::Ctrl && (event.sym == XKB_KEY_1 || event.sym == XKB_KEY_2)) {
          setViewMode(event.sym == XKB_KEY_1 ? files::ViewMode::GRID : files::ViewMode::LIST);
          return;
        }
        if (!editing && event.sym == XKB_KEY_h && event.modifiers == KeyMod::Ctrl) {
          m_browser.toggleHidden();
          return;
        }
        if (m_list != nullptr && m_dispatcher.focusedArea() == m_list->focusArea() && listKey(event)) {
          return;
        }
      }
      if (event.pressed
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
    struct PinDrag {
      std::vector<EntryTarget> entries;
      std::vector<files::Bookmark> bookmarks;
      std::optional<files::Location> sourcePin;
    };

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
      if (!id) {
        const auto location = m_browser.model().snapshot().location;
        std::vector<ContextMenuControlEntry> entries{{.id = 1, .label = "Refresh"}};
        if (location) {
          entries.push_back(
              {.id = 2,
               .label = m_pins.contains(*location) ? "Unpin" : "Pin this folder to sidebar",
               .enabled = m_pins.editable()}
          );
        }
        entries.push_back({.label = "", .separator = true});
        entries.push_back({.id = 3, .label = "Reload pins", .enabled = !m_pins.busy()});
        showContextMenu(
            std::move(entries),
            [this, location](int action) {
              if (action == 1) {
                m_browser.refresh();
              } else if (action == 3) {
                m_pins.reload();
              } else if (action == 2 && location) {
                if (m_pins.contains(*location)) {
                  m_pins.remove(*location);
                } else {
                  pinFolders(
                      {{.label = files::locationBreadcrumbs(*location).back().label, .location = *location}},
                      m_pins.bookmarks().size()
                  );
                }
              }
            },
            {x, y, 1, 1}
        );
        return;
      }
      std::vector<files::DirectoryEntry> targets;
      for (const auto selected : m_browser.model().selectedIds()) {
        if (const auto* entry = m_browser.model().entry(selected)) {
          targets.push_back(*entry);
        }
      }
      if (targets.empty()) {
        return;
      }
      const bool folders =
          std::ranges::all_of(targets, [](const auto& entry) { return entry.type == G_FILE_TYPE_DIRECTORY; });
      const bool files = std::ranges::all_of(targets, [](const auto& entry) {
        return entry.type == G_FILE_TYPE_REGULAR && !entry.symlink;
      });
      std::vector<ContextMenuControlEntry> entries;
      if (targets.size() == 1 || files) {
        entries.push_back({.id = 1, .label = "Open"});
      }
      if (targets.size() == 1) {
        entries.push_back({.id = 2, .label = "Show in containing folder"});
      }
      if (folders) {
        const bool canPin =
            std::ranges::any_of(targets, [this](const auto& entry) { return !m_pins.contains(entry.location); });
        const bool canUnpin =
            std::ranges::any_of(targets, [this](const auto& entry) { return m_pins.contains(entry.location); });
        if (canPin || canUnpin) {
          entries.push_back({.label = "", .separator = true});
        }
        if (canPin) {
          entries.push_back({.id = 3, .label = "Pin to sidebar", .enabled = m_pins.editable()});
        }
        if (canUnpin) {
          entries.push_back(
              {.id = 4, .label = targets.size() == 1 ? "Unpin" : "Unpin folders", .enabled = m_pins.editable()}
          );
        }
      }
      entries.push_back({.label = "", .separator = true});
      entries.push_back({.id = 5, .label = "Properties"});
      showContextMenu(
          std::move(entries),
          [this, targets](int action) {
            if (!targetsCurrent(targets)) {
              return;
            }
            if (action == 1) {
              for (const auto& target : targets) {
                m_browser.activate(target.id);
              }
            } else if (action == 2) {
              showContaining(targets.front().location);
            } else if (action == 3) {
              std::vector<files::Bookmark> bookmarks;
              for (const auto& target : targets) {
                if (!m_pins.contains(target.location)) {
                  bookmarks.push_back({target.displayName, target.location});
                }
              }
              pinFolders(std::move(bookmarks), m_pins.bookmarks().size());
            } else if (action == 4) {
              std::vector<files::Location> locations;
              locations.reserve(targets.size());
              for (const auto& target : targets) {
                locations.push_back(target.location);
              }
              m_pins.removeMany(locations);
            } else if (action == 5) {
              showProperties(targets);
            }
          },
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
        entries.push_back({.id = 2, .label = "Unpin", .enabled = m_pins.editable()});
        entries.push_back({.id = 6, .label = "Rename place…", .enabled = m_pins.editable()});
        entries.push_back({.id = 3, .label = "Move up", .enabled = m_pins.editable() && index > 0});
        entries.push_back({.id = 4, .label = "Move down", .enabled = m_pins.editable() && index + 1 < pins.size()});
      }
      entries.push_back({.label = "", .separator = true});
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

    void openPlaceRename(const files::Bookmark& bookmark) {
      const auto found = std::ranges::find(m_pins.bookmarks(), bookmark.location, &files::Bookmark::location);
      if (!m_pins.editable() || found == m_pins.bookmarks().end()) {
        actionError("This place is unavailable for editing. Reload Places and try again.");
        return;
      }
      const auto current = *found; // NOLINT(performance-unnecessary-copy-initialization): own the editing snapshot.
      const float width = std::min(440.0F, static_cast<float>(m_surface.width()) - 32.0F);
      auto editor = std::make_unique<files::PlaceLabelEditor>(
          width, current.label, [this](std::string label) { submitPlaceRename(std::move(label)); },
          [this] { dismissMenu(); }
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

    bool pinDragCurrent() const {
      if (!m_pinDrag || !m_pins.editable()) {
        return false;
      }
      if (m_pinDrag->sourcePin) {
        return m_pins.contains(*m_pinDrag->sourcePin);
      }
      return std::ranges::all_of(m_pinDrag->entries, [this](const auto& target) {
        const auto* entry = m_browser.model().entry(target.id);
        return entry != nullptr
            && entry->location == target.location
            && entry->type == G_FILE_TYPE_DIRECTORY
            && std::ranges::contains(m_browser.model().rows(), target.id);
      });
    }

    void startFilePinDrag(const std::vector<files::EntryId>& ids) {
      if (ids.empty() || !m_pins.editable()) {
        return;
      }
      PinDrag drag;
      for (const auto id : ids) {
        const auto* entry = m_browser.model().entry(id);
        if (entry == nullptr || entry->type != G_FILE_TYPE_DIRECTORY) {
          return;
        }
        drag.entries.push_back({id, entry->location});
        drag.bookmarks.push_back({entry->displayName, entry->location});
      }
      m_pinDrag = std::move(drag);
      updatePinDrag();
    }
    void startPlacePinDrag(const files::Bookmark& bookmark) {
      if (!m_pins.editable()) {
        return;
      }
      m_pinDrag = PinDrag{.entries = {}, .bookmarks = {bookmark}, .sourcePin = bookmark.location};
      updatePinDrag();
    }
    void cancelPinDrag() {
      if (!m_pinDrag) {
        return;
      }
      m_pinDrag.reset();
      m_pinDragScroll.stop();
      m_pinDragScrollDelta = 0;
      if (m_places != nullptr) {
        m_places->cancelDragCandidate();
      }
      m_dispatcher.cancelPointerCapture();
      if (m_dragPreview != nullptr) {
        m_dragPreview->setVisible(false);
        m_pinDropLine->setVisible(false);
      }
      m_surface.requestLayout();
    }
    void updatePinDrag() {
      if (!pinDragCurrent()) {
        cancelPinDrag();
        return;
      }
      const auto slot = m_places->dropSlot(m_pointerX, m_pointerY);
      const float scroll = slot ? m_places->dragScrollSpeed(m_pointerX, m_pointerY) : 0.0F;
      if (scroll == 0) {
        m_pinDragScroll.stop();
        m_pinDragScrollDelta = 0;
      } else if (!m_pinDragScroll.active()) {
        m_pinDragScroll.startRepeating(std::chrono::milliseconds(16), [this] {
          if (!pinDragCurrent()) {
            cancelPinDrag();
            return;
          }
          const auto slot = m_places->dropSlot(m_pointerX, m_pointerY);
          const float step = slot ? m_places->dragScrollSpeed(m_pointerX, m_pointerY) : 0.0F;
          if (step == 0) {
            m_pinDragScroll.stop();
            m_pinDragScrollDelta = 0;
            return;
          }
          m_pinDragScrollDelta = std::clamp(m_pinDragScrollDelta + step, -18.0F, 18.0F);
          m_surface.requestLayout();
        });
      }
      m_surface.requestLayout();
    }
    void layoutPinDrag(Renderer& renderer) {
      if (m_pinDrag && !pinDragCurrent()) {
        cancelPinDrag();
      }
      m_dragPreview->setVisible(m_pinDrag.has_value());
      m_pinDropLine->setVisible(false);
      if (!m_pinDrag) {
        return;
      }
      m_dragLabel->setText(
          m_pinDrag->sourcePin
              ? "Reorder place"
              : std::format("Pin {} folder{}", m_pinDrag->bookmarks.size(), m_pinDrag->bookmarks.size() == 1 ? "" : "s")
      );
      m_dragPreview->arrange(
          renderer,
          {std::clamp(m_pointerX + 12, 0.0F, static_cast<float>(m_surface.width()) - 190.0F),
           std::clamp(m_pointerY + 12, 0.0F, static_cast<float>(m_surface.height()) - 38.0F), 190, 38}
      );
      if (const auto slot = m_places->dropSlot(m_pointerX, m_pointerY); slot) {
        m_pinDropLine->setVisible(true);
        m_pinDropLine->arrange(renderer, m_places->dropLine(*slot));
      }
    }
    void finishPinDrag(float x, float y) {
      const bool current = pinDragCurrent();
      const auto slot = m_places->dropSlot(x, y);
      const auto drag = *m_pinDrag;
      cancelPinDrag();
      if (current && slot) {
        pinFolders(drag.bookmarks, *slot);
      }
    }
    TextInputState textInputState() const override { return m_filterInput->textInputState(); }
    void textInputApplyEdit(const TextInputEdit& edit) override {
      if ((edit.hasCommitText && !edit.commitText.empty())
          || (edit.hasPreedit && !edit.preeditText.empty())
          || (edit.hasDelete && !m_browser.searchQuery().empty())) {
        openFilter(false);
        m_filterInput->textInputApplyEdit(edit);
      }
    }
    void textInputResetPreedit() override {}
    void textInputActivated(TextInputService&) override {}
    void textInputDeactivated(TextInputService&) override {}

    void beginLocationEdit() {
      m_lastBreadcrumb.reset();
      m_breadcrumbEditTarget.reset();
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
    void rebuildBreadcrumbs() {
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
                       button.label()->setUseMarkup(false);
                       button.inputArea()->setOnClick([this, target](const InputArea::PointerData& pointer) {
                         if (pointer.button == BTN_LEFT) {
                           breadcrumbClick(target, &pointer);
                         }
                       });
                     }}
            )
        );
      }
      m_breadcrumbScroll->requestScrollToBottom();
    }

    void openFilter(bool selectAll = true) {
      m_lastBreadcrumb.reset();
      m_breadcrumbEditTarget.reset();
      if (m_editingLocation) {
        endLocationEdit(false);
      }
      m_filterOpen = true;
      if (m_filterInput->value() != m_browser.searchQuery()) {
        m_filterInput->setValue(m_browser.searchQuery());
      }
      m_dispatcher.setFocus(m_filterInput->inputArea());
      if (selectAll) {
        m_filterInput->selectAll();
      }
      m_browserDirty = true;
      m_surface.requestUpdate();
    }

    void changeNameFilter(const std::string& text) {
      m_filterOpen = true;
      if (text == m_browser.searchQuery()) {
        return;
      }
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      m_viewAnchor.reset();
      m_list->scrollView().requestScrollToOffset(0);
      m_browser.setSearchQuery(text);
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

    void changeSort(files::SortOptions options) {
      if (options == m_browser.model().sort()) {
        return;
      }
      captureViewAnchor();
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      m_browser.model().setSort(options);
      m_dispatcher.setFocus(m_list->focusArea());
    }

    void sortBy(files::SortField field) {
      auto options = m_browser.model().sort();
      options.descending = options.field == field && !options.descending;
      options.field = field;
      changeSort(options);
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
      const bool search = !m_browser.searchQuery().empty();
      return m_viewMode == files::ViewMode::GRID ? gridIconSize() + (search ? 72.0F : 52.0F) : (search ? 52.0F : 36.0F);
    }

    float gridIconSize() const { return static_cast<float>(files::kGridIconSizes[m_gridZoom]); }

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
      m_zoomScroll = std::clamp(m_zoomScroll - steps, -6.0, 6.0);
      const auto delta = static_cast<int>(m_zoomScroll);
      m_zoomScroll -= delta;
      const auto zoom = static_cast<std::size_t>(std::clamp(static_cast<int>(m_gridZoom) + delta, 0, 5));
      if (zoom != m_gridZoom) {
        captureViewAnchor();
        m_listAdapter.onPointerCancel();
        m_gridAdapter.onPointerCancel();
        m_gridZoom = zoom;
        m_preferences.saveGridIconSize(files::kGridIconSizes[m_gridZoom]);
        m_browserDirty = true;
        m_surface.requestUpdate();
      }
      return true;
    }

    bool listKey(const KeyboardEvent& event) {
      auto& model = m_browser.model();
      if (event.sym == XKB_KEY_a && event.modifiers == KeyMod::Ctrl) {
        model.selectAll();
        return true;
      }
      if (event.modifiers == 0 && KeySymbol::isEscape(event.sym)) {
        m_list->cancelPointerGesture();
        model.clearSelection();
        return true;
      }
      if (event.modifiers == 0 && KeySymbol::isEnter(event.sym)) {
        if (!event.repeat && model.selection().focused) {
          m_browser.activate(*model.selection().focused);
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
      m_list->scrollToIndex(row);
      return true;
    }

    void syncBrowser() {
      const auto& model = m_browser.model();
      const auto& snapshot = model.snapshot();
      const auto uri = snapshot.location ? snapshot.location->uri() : std::string{};
      if (uri != m_displayedLocation) {
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
      m_location->setInvalid(m_locationInvalid);
      if (m_filterInput->value() != m_browser.searchQuery()) {
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
      const auto& sort = model.sort();
      const char* indicator = sort.descending ? "arrow-down" : "arrow-up";
      m_nameHeader->setGlyph(indicator);
      m_sizeHeader->setGlyph(indicator);
      m_nameHeader->glyph()->setVisible(sort.field == files::SortField::NAME);
      m_nameHeader->glyph()->setParticipatesInLayout(sort.field == files::SortField::NAME);
      m_sizeHeader->glyph()->setVisible(sort.field == files::SortField::SIZE);
      m_sizeHeader->glyph()->setParticipatesInLayout(sort.field == files::SortField::SIZE);
      const char* field = sort.field == files::SortField::NAME ? "Name"
          : sort.field == files::SortField::SIZE               ? "Size"
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
      m_back->setEnabled(m_browser.canBack());
      m_forward->setEnabled(m_browser.canForward());
      m_up->setEnabled(m_browser.canUp());
      m_places->sync(snapshot.location);
      const bool loading = snapshot.status == files::DirectoryStatus::LOADING;
      m_refresh->setGlyph(loading ? "square" : "refresh");
      m_refresh->setTooltip(loading ? "Stop loading" : "Refresh");
      std::string status;
      if (!m_actionError.empty()) {
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
      if (!model.selection().selected.empty()) {
        status += std::format(" · {} selected", model.selection().selected.size());
      }
      m_status->setText(status);
      m_notice->setText(
          snapshot.error
                  || loading
                  || snapshot.status == files::DirectoryStatus::CANCELLED
                  || !m_browser.message().empty()
                  || !m_preferenceError.empty()
              ? status
              : !m_browser.searchQuery().empty() ? "No matching files or folders."
              : snapshot.entries.empty()         ? "This folder is empty."
                                                 : "No visible items."
      );
      const bool empty = model.rows().empty();
      m_notice->setVisible(empty);
      m_notice->setParticipatesInLayout(empty);
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
        if (m_pinDrag && m_pinDragScrollDelta != 0) {
          m_places->dragScroll(std::exchange(m_pinDragScrollDelta, 0.0F));
        }
        const float searchWidth = m_surface.width() < 800 ? 148.0F : 220.0F;
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
        const bool clientHeader = !m_surface.serverDecorated() && !m_surface.fullscreen();
        m_titlebar->setVisible(clientHeader);
        m_titlebar->setParticipatesInLayout(clientHeader);
        m_root->arrange(
            renderer, LayoutRect{0, 0, static_cast<float>(m_surface.width()), static_cast<float>(m_surface.height())}
        );
        layoutPinDrag(renderer);
        if (placesFocus) {
          m_dispatcher.restoreTabFocus(std::move(*placesFocus));
        }
        if (m_restorePlaceFocus && m_menuSurface == nullptr) {
          const auto location = *std::exchange(m_restorePlaceFocus, std::nullopt);
          if (auto* area = m_places->focusArea(location)) {
            m_dispatcher.setFocus(area);
          }
        }
        if (m_dispatcher.focusedArea() == m_list->focusArea()) {
          m_textInput.notifyClientStateChanged(this, TextInputChangeCause::Other);
        }
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
                     if (!m_browser.model().rows().empty()) {
                       m_dispatcher.setFocus(m_list->focusArea());
                     }
                   },
               .configure =
                   [this](Input& input) {
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
      auto
          toolbar =
              ui::row(
                  {.align = FlexAlign::Center, .gap = Style::spaceSm, .padding = Style::spaceSm},
                  ui::row(
                      {.align = FlexAlign::Center, .gap = Style::spaceXs},
                      ui::button(
                          {.out = &m_back,
                           .glyph = "arrow-left",
                           .variant = ButtonVariant::Ghost,
                           .tooltip = "Back (Alt+Left)",
                           .onClick = [this] { (void)m_browser.back(); }}
                      ),
                      ui::button(
                          {.out = &m_forward,
                           .glyph = "arrow-right",
                           .variant = ButtonVariant::Ghost,
                           .tooltip = "Forward (Alt+Right)",
                           .onClick = [this] { (void)m_browser.forward(); }}
                      ),
                      ui::button(
                          {.out = &m_up,
                           .glyph = "arrow-up",
                           .variant = ButtonVariant::Ghost,
                           .tooltip = "Parent folder (Alt+Up)",
                           .onClick = [this] { (void)m_browser.up(); }}
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
                                   m_browser.stop();
                                 } else {
                                   m_browser.refresh();
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
                           .onChange = [this](
                                           std::size_t index
                                       ) { setViewMode(index == 0 ? files::ViewMode::GRID : files::ViewMode::LIST); }}
                      ),
                      ui::button(
                          {.out = &m_menuButton,
                           .glyph = "dots-vertical",
                           .variant = ButtonVariant::Ghost,
                           .onClick = [this] { openMenu(); }}
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
      places->setDragCallback([this](const files::Bookmark& bookmark) { startPlacePinDrag(bookmark); });
      places->setRenameCallback([this](const files::Bookmark& bookmark) { openPlaceRename(bookmark); });
      auto content = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs, .padding = Style::spaceSm, .flexGrow = 1.0F},
          ui::column(
              {.out = &m_listHeader, .align = FlexAlign::Stretch},
              ui::row(
                  {.height = 32.0F},
                  ui::row(
                      {.align = FlexAlign::Center, .gap = Style::spaceSm, .paddingH = Style::spaceMd, .flexGrow = 1.0F},
                      ui::button(
                          {.out = &m_nameHeader,
                           .text = "Name",
                           .fontSize = Style::fontSizeCaption,
                           .controlHeight = Style::controlHeightSm,
                           .contentAlign = ButtonContentAlign::Start,
                           .variant = ButtonVariant::Ghost,
                           .tooltip = "Sort by name; click again to reverse",
                           .paddingH = 0.0F,
                           .flexGrow = 1.0F,
                           .onClick = [this] { sortBy(files::SortField::NAME); }}
                      ),
                      ui::label(
                          {.text = "Type", .fontSize = Style::fontSizeCaption, .minWidth = 100.0F, .maxWidth = 100.0F}
                      ),
                      ui::button(
                          {.out = &m_sizeHeader,
                           .text = "Size",
                           .fontSize = Style::fontSizeCaption,
                           .controlHeight = Style::controlHeightSm,
                           .contentAlign = ButtonContentAlign::End,
                           .variant = ButtonVariant::Ghost,
                           .tooltip = "Sort by size; click again to reverse",
                           .minWidth = 110.0F,
                           .maxWidth = 110.0F,
                           .paddingH = 0.0F,
                           .onClick = [this] { sortBy(files::SortField::SIZE); }}
                      )
                  ),
                  ui::box({.width = Style::scrollbarWidth + Style::scrollbarGap})
              ),
              ui::separator({})
          ),
          ui::label({.out = &m_notice, .textAlign = TextAlign::Center, .flexGrow = 1.0F}),
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
      m_root = ui::column(
          {.align = FlexAlign::Stretch,
           .fill = colorSpecFromRole(ColorRole::Surface),
           .fillWidth = true,
           .fillHeight = true},
          std::move(header), std::move(toolbar),
          ui::row(
              {.align = FlexAlign::Stretch, .clipChildren = true, .flexGrow = 1.0F}, std::move(places),
              std::move(content)
          ),
          ui::row(
              {.align = FlexAlign::Center,
               .padding = Style::spaceSm,
               .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow)},
              ui::label({.out = &m_status, .maxLines = 1, .flexGrow = 1.0F})
          )
      );
      m_root->addChild(
          ui::column(
              {.out = &m_dragPreview,
               .padding = 8.0F,
               .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
               .radius = Style::radiusMd,
               .border = colorSpecFromRole(ColorRole::Primary),
               .width = 190.0F,
               .height = 38.0F,
               .visible = false,
               .participatesInLayout = false},
              ui::label({.out = &m_dragLabel, .maxWidth = 174.0F, .maxLines = 1, .configure = [](Label& label) {
                           label.setUseMarkup(false);
                         }})
          )
      );
      m_dragPreview->setZIndex(100);
      m_dragPreview->setHitTestVisible(false);
      m_root->addChild(
          ui::box(
              {.out = &m_pinDropLine,
               .fill = colorSpecFromRole(ColorRole::Primary),
               .height = 2.0F,
               .visible = false,
               .participatesInLayout = false}
          )
      );
      m_pinDropLine->setZIndex(101);
      m_pinDropLine->setHitTestVisible(false);
      m_root->setAnimationManager(&m_animations);
      m_surface.setSceneRoot(m_root.get());
      m_dispatcher.setSceneRoot(m_root.get());
      m_dispatcher.setTextInputContext(m_surface.wlSurface(), &m_textInput);
      m_dispatcher.setFocus(m_list->focusArea());
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
      if (m_placeRenameTarget) {
        m_restorePlaceFocus = m_placeRenameTarget->location;
      }
      m_placeEditor = nullptr;
      m_placeRenameTarget.reset();
      m_pendingPlaceLabel.reset();
      m_menuDispatcher.setSceneRoot(nullptr);
      m_menuDispatcher.setTextInputContext(nullptr, nullptr);
      if (m_menuSurface != nullptr) {
        m_menuSurface->setSceneRoot(nullptr);
      }
      m_menuRoot.reset();
      m_menu = nullptr;
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
        bool alignLeft = false
    ) {
      cancelPinDrag();
      auto menu = std::make_unique<ContextMenuControl>();
      auto* control = menu.get();
      const float height = ContextMenuControl::preferredHeight(entries, entries.size(), 1.0F);
      constexpr float width = 264.0F;
      control->setEntries(std::move(entries));
      control->setMenuWidth(width);
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
      auto root = ui::column(
          {.padding = 8.0F, .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh), .radius = Style::radiusLg},
          std::move(menu)
      );
      showPopup(std::move(root), control, anchor, width + 16.0F, height + 16.0F, alignLeft);
    }

    void openMenu() {
      if (m_menuSurface != nullptr) {
        dismissMenu();
        return;
      }
      const auto& sort = m_browser.model().sort();
      std::vector<ContextMenuControlEntry> entries{
          {.label = "Sort by", .header = true},
          {.id = 1, .label = "Name", .radio = true, .toggleState = sort.field == files::SortField::NAME ? 1 : 0},
          {.id = 2, .label = "Size", .radio = true, .toggleState = sort.field == files::SortField::SIZE ? 1 : 0},
          {.id = 3,
           .label = "Modified",
           .radio = true,
           .toggleState = sort.field == files::SortField::MODIFIED ? 1 : 0},
          {.label = "", .separator = true},
          {.id = 4, .label = "Ascending", .radio = true, .toggleState = sort.descending ? 0 : 1},
          {.id = 5, .label = "Descending", .radio = true, .toggleState = sort.descending ? 1 : 0},
          {.label = "", .separator = true},
          {.id = 6, .label = "Folders first", .checkmark = true, .toggleState = sort.directoriesFirst ? 1 : 0},
          {.id = 7,
           .label = "Show hidden files",
           .checkmark = true,
           .toggleState = m_browser.model().filter().showHidden ? 1 : 0},
          {.label = "", .separator = true},
          {.id = 8, .label = "Search files (Ctrl+F)"},
      };
      showContextMenu(
          std::move(entries),
          [this](int id) {
            if (id <= 6) {
              auto options = m_browser.model().sort();
              if (id >= 1 && id <= 3) {
                const auto field = id == 1 ? files::SortField::NAME
                    : id == 2              ? files::SortField::SIZE
                                           : files::SortField::MODIFIED;
                if (field != options.field) {
                  options.field = field;
                  options.descending = false;
                }
              } else if (id == 4 || id == 5) {
                options.descending = id == 5;
              } else if (id == 6) {
                options.directoriesFirst = !options.directoriesFirst;
              }
              changeSort(options);
            } else if (id == 7) {
              m_browser.toggleHidden();
            } else if (id == 8) {
              openFilter();
              return;
            }
            m_dispatcher.setFocus(m_list->focusArea());
          },
          sceneRect(*m_menuButton), true
      );
    }

    WaylandConnection& m_connection;
    RenderContext& m_renderer;
    TextInputService& m_textInput;
    AnimationManager m_animations;
    ToplevelSurface m_surface;
    files::BrowserController m_browser;
    files::FileIconCache m_icons;
    files::FileViewAdapter m_listAdapter;
    files::FileViewAdapter m_gridAdapter;
    files::ViewPreferenceWriter m_preferences;
    files::PinService m_pins;
    files::FileLauncher m_launcher;
    files::PlacesSidebar* m_places = nullptr;
    bool m_placesDirty = true;
    std::string m_actionError;
    std::optional<files::Location> m_revealTarget;
    std::optional<PinDrag> m_pinDrag;
    Timer m_pinDragScroll;
    float m_pinDragScrollDelta = 0;
    float m_pointerX = 0;
    float m_pointerY = 0;
    std::uint32_t m_popupSerial = 0;
    Flex* m_dragPreview = nullptr;
    Label* m_dragLabel = nullptr;
    Box* m_pinDropLine = nullptr;
    std::string m_preferenceError;
    std::size_t m_gridZoom = 1;
    double m_zoomScroll = 0;
    files::ViewMode m_viewMode;
    std::optional<files::ViewMode> m_appliedViewMode;
    std::optional<files::EntryId> m_viewAnchor;
    std::unique_ptr<Node> m_root;
    InputDispatcher m_dispatcher;
    Flex* m_titlebar = nullptr;
    Input* m_location = nullptr;
    Input* m_filterInput = nullptr;
    Flex* m_searchArea = nullptr;
    Flex* m_locationArea = nullptr;
    ScrollView* m_breadcrumbScroll = nullptr;
    Flex* m_breadcrumbs = nullptr;
    std::optional<files::Location> m_lastBreadcrumb;
    std::optional<files::Location> m_breadcrumbEditTarget;
    std::chrono::steady_clock::time_point m_breadcrumbClickTime;
    float m_breadcrumbClickX = 0;
    float m_breadcrumbClickY = 0;
    bool m_breadcrumbsDirty = true;
    bool m_editingLocation = false;
    bool m_filterOpen = false;
    Label* m_status = nullptr;
    Label* m_notice = nullptr;
    VirtualGridView* m_list = nullptr;
    Flex* m_listHeader = nullptr;
    Segmented* m_viewSwitch = nullptr;
    Button* m_nameHeader = nullptr;
    Button* m_sizeHeader = nullptr;
    Button* m_menuButton = nullptr;
    Button* m_back = nullptr;
    Button* m_forward = nullptr;
    Button* m_up = nullptr;
    Button* m_refresh = nullptr;
    std::string m_displayedLocation;
    bool m_browserDirty = true;
    bool m_locationInvalid = false;
    std::unique_ptr<PopupSurface> m_menuSurface;
    std::unique_ptr<Flex> m_menuRoot;
    ContextMenuControl* m_menu = nullptr;
    files::PlaceLabelEditor* m_placeEditor = nullptr;
    std::optional<files::Bookmark> m_placeRenameTarget;
    std::optional<std::string> m_pendingPlaceLabel;
    std::optional<files::Location> m_restorePlaceFocus;
    InputDispatcher m_menuDispatcher;
    std::uint64_t m_menuGeneration = 0;
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
    GlSharedContext graphics;
    graphics.initialize(connection.display());
    RenderContext renderer;
    renderer.initialize(graphics);
    Window window(connection, renderer, textInput, config.settings().browser, ConfigPaths::fromEnvironment());
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
