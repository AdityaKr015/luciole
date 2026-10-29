#include "app/application.h"

#include "app/deferred_call_poll_source.h"
#include "app/glib_poll_source.h"
#include "app/main_loop.h"
#include "app/timer_poll_source.h"
#include "browser/browser_controller.h"
#include "browser/details_columns.h"
#include "browser/file_view.h"
#include "browser/location_presentation.h"
#include "browser/places_sidebar.h"
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
#include "operations/create_service.h"
#include "operations/empty_trash_service.h"
#include "operations/rename_service.h"
#include "operations/transfer_service.h"
#include "operations/trash_service.h"
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
#include "ui/controls/name_editor.h"
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
              [this] { return m_fileKeyboardFocus && m_list != nullptr && m_list->focusArea()->focused(); }
          ),
          m_gridAdapter(
              m_browser, m_icons, files::ViewMode::GRID, [this] { return m_connection.input().keyboardModifiers(); },
              [this] { return m_fileKeyboardFocus && m_list != nullptr && m_list->focusArea()->focused(); }
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
        m_preferenceError = error.empty() ? "" : std::format("Could not save the view preference: {}", error);
        m_browserDirty = true;
        m_surface.requestUpdate();
      });
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
      m_pointerX = x;
      m_pointerY = y;
      if (event.type == PointerEvent::Type::Button && event.pressed) {
        m_popupSerial = event.serial;
        if (dispatcher == &m_dispatcher) {
          m_fileKeyboardFocus = false;
          m_browserDirty = true;
          m_surface.requestUpdate();
        }
      }
      if (dispatcher == &m_dispatcher
          && event.type == PointerEvent::Type::Button
          && event.pressed
          && event.button == BTN_RIGHT
          && m_notice != nullptr
          && m_notice->visible()
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
        if (m_drag && KeySymbol::isEscape(event.sym)) {
          cancelDrag();
          return;
        }
      }
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
            && (event.sym == XKB_KEY_n || event.sym == XKB_KEY_N)
            && event.modifiers == (KeyMod::Ctrl | KeyMod::Shift)
            && !event.repeat) {
          if (const auto parent = m_browser.model().snapshot().location)
            openCreateItem(*parent, files::CreateKind::FOLDER);
          return;
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
              (void)m_list->absoluteAnchorForIndex(static_cast<std::size_t>(found - rows.begin()), x, y);
            }
          }
          openFileMenu(id, x, y);
          return;
        }
        if (!editing
            && event.sym == XKB_KEY_Delete
            && event.modifiers == 0
            && !event.repeat
            && m_list != nullptr
            && m_dispatcher.focusedArea() == m_list->focusArea()) {
          std::vector<files::DirectoryEntry> targets;
          for (const auto id : m_browser.model().selectedIds()) {
            if (const auto* entry = m_browser.model().entry(id))
              targets.push_back(*entry);
          }
          if (!targets.empty())
            trashItems(targets);
          return;
        }
        if (!editing
            && event.sym == XKB_KEY_F2
            && event.modifiers == 0
            && !event.repeat
            && m_list != nullptr
            && m_dispatcher.focusedArea() == m_list->focusArea()) {
          const auto ids = m_browser.model().selectedIds();
          if (ids.size() == 1) {
            if (const auto* entry = m_browser.model().entry(ids.front()))
              openFileRename(*entry);
          }
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
          m_fileKeyboardFocus = !KeySymbol::isEscape(event.sym);
          m_browserDirty = true;
          m_surface.requestUpdate();
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
        std::vector<ContextMenuControlEntry> entries{
            {.id = 4, .label = "New Folder…", .enabled = canCreateItem()},
            {.id = 5, .label = "Empty File…", .enabled = canCreateItem()},
            {.label = "", .separator = true},
            {.id = 1, .label = "Refresh"}
        };
        if (location && !location->isTrash()) {
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
              if ((action == 4 || action == 5) && location) {
                openCreateItem(*location, action == 4 ? files::CreateKind::FOLDER : files::CreateKind::EMPTY_FILE);
              } else if (action == 1) {
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
      const bool trash = targets.front().location.isTrash();
      if ((targets.size() == 1 || files)
          && (!trash || (targets.front().type == G_FILE_TYPE_DIRECTORY && !targets.front().symlink))) {
        entries.push_back({.id = 1, .label = "Open"});
      }
      if (targets.size() == 1) {
        entries.push_back({.id = 2, .label = "Show in containing folder"});
      }
      if (targets.size() == 1 && targets.front().trash.original)
        entries.push_back({.id = 8, .label = "Show original location"});
      if (folders && !trash) {
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
      if (targets.size() == 1 && !trash) {
        entries.push_back({.id = 6, .label = "Rename…", .enabled = canFileRename(targets.front())});
      }
      if (!trash)
        entries.push_back({.id = 7, .label = "Move to Trash", .enabled = canTrashItems(targets)});
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
            } else if (action == 8 && targets.front().trash.original) {
              showContaining(*targets.front().trash.original);
            } else if (action == 7) {
              trashItems(targets);
            } else if (action == 6) {
              openFileRename(targets.front());
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
        if (!bookmark.location.isTrash()) {
          entries.push_back({.id = 2, .label = "Unpin", .enabled = m_pins.editable()});
          entries.push_back({.id = 6, .label = "Rename place…", .enabled = m_pins.editable()});
        }
        entries.push_back({.id = 3, .label = "Move up", .enabled = m_pins.editable() && index > 0});
        entries.push_back({.id = 4, .label = "Move down", .enabled = m_pins.editable() && index + 1 < pins.size()});
      }
      entries.push_back({.label = "", .separator = true});
      if (bookmark.location == files::Location::trash()) {
        entries.push_back({.id = 7, .label = "Empty Trash…", .enabled = canEmptyTrash(), .destructive = true});
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
              openEmptyTrash();
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
      const auto& snapshot = m_browser.model().snapshot();
      return !targets.empty()
          && snapshot.location
          && snapshot.location->nativePath().has_value()
          && m_browser.searchQuery().empty()
          && (snapshot.status == files::DirectoryStatus::READY || snapshot.status == files::DirectoryStatus::LOADING)
          && snapshot.metadata.filesystem.readOnly != true
          && !m_transfers.busy()
          && !m_trash.busy()
          && !m_trash.canTrash()
          && !m_emptyTrash.pending()
          && !m_renamer.busy()
          && !m_creator.busy()
          && std::ranges::all_of(targets, [](const auto& entry) {
               return entry.location.nativePath().has_value() && entry.access.trash != false;
             });
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

    bool canCreateItem() const {
      const auto& snapshot = m_browser.model().snapshot();
      return snapshot.location
          && snapshot.location->nativePath().has_value()
          && m_browser.searchQuery().empty()
          && (snapshot.status == files::DirectoryStatus::READY || snapshot.status == files::DirectoryStatus::LOADING)
          && snapshot.metadata.filesystem.readOnly != true
          && snapshot.metadata.access.write != false
          && !m_creator.busy()
          && !m_renamer.busy()
          && !m_transfers.busy()
          && !m_trash.busy()
          && !m_trash.canTrash()
          && !m_emptyTrash.pending();
    }
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
      return entry.location.nativePath().has_value()
          && entry.access.rename != false
          && !m_transfers.busy()
          && !m_trash.busy()
          && !m_trash.canTrash()
          && !m_emptyTrash.pending()
          && !m_renamer.busy()
          && !m_creator.busy();
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
      if (const auto place = m_places->folderAt(m_pointerX, m_pointerY)) {
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
      if (!m_drag || !m_pins.editable()) {
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
      places = m_places->containsScenePoint(m_pointerX, m_pointerY);
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
      const auto& location = m_browser.model().snapshot().location;
      const bool context = !m_browser.searchQuery().empty() || (location && location->isTrash());
      return m_viewMode == files::ViewMode::GRID ? gridIconSize() + (context ? 72.0F : 52.0F)
                                                 : (context ? 52.0F : 36.0F);
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
        if (!event.repeat
            && model.selection().focused
            && (m_fileKeyboardFocus || model.selection().selected.contains(*model.selection().focused))) {
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
      m_modifiedHeader->setGlyph(indicator);
      m_nameHeader->glyph()->setVisible(sort.field == files::SortField::NAME);
      m_nameHeader->glyph()->setParticipatesInLayout(sort.field == files::SortField::NAME);
      m_sizeHeader->glyph()->setVisible(sort.field == files::SortField::SIZE);
      m_sizeHeader->glyph()->setParticipatesInLayout(sort.field == files::SortField::SIZE);
      m_modifiedHeader->glyph()->setVisible(sort.field == files::SortField::MODIFIED);
      m_modifiedHeader->glyph()->setParticipatesInLayout(sort.field == files::SortField::MODIFIED);
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
      }
      m_status->setText(status);
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
        const auto columns = files::DetailsColumns::forWidth(m_list->scrollView().contentViewportWidth(true));
        if (columns != m_detailsColumns) {
          m_detailsColumns = columns;
          m_typeHeader->setVisible(columns.showType);
          m_typeHeader->setParticipatesInLayout(columns.showType);
          m_sizeHeader->setMinWidth(columns.sizeWidth + 2 * Style::spaceXs);
          m_sizeHeader->setMaxWidth(columns.sizeWidth + 2 * Style::spaceXs);
          m_modifiedHeader->setMinWidth(columns.modifiedWidth + 2 * Style::spaceXs);
          m_modifiedHeader->setMaxWidth(columns.modifiedWidth + 2 * Style::spaceXs);
          m_root->arrange(
              renderer, LayoutRect{0, 0, static_cast<float>(m_surface.width()), static_cast<float>(m_surface.height())}
          );
        }
        layoutDrag(renderer);
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
      places->setDragCallback([this](const files::Bookmark& bookmark) { startPlaceDrag(bookmark); });
      places->setRenameCallback([this](const files::Bookmark& bookmark) { openPlaceRename(bookmark); });
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
                      {.align = FlexAlign::Center, .paddingH = Style::spaceMd - Style::spaceXs, .flexGrow = 1.0F},
                      ui::button(
                          {.out = &m_nameHeader,
                           .text = "Name",
                           .fontSize = Style::fontSizeCaption,
                           .controlHeight = Style::controlHeightSm,
                           .contentAlign = ButtonContentAlign::Start,
                           .variant = ButtonVariant::Ghost,
                           .tooltip = "Sort by name; click again to reverse",
                           .paddingH = Style::spaceXs,
                           .flexGrow = 1.0F,
                           .onClick = [this] { sortBy(files::SortField::NAME); }}
                      ),
                      ui::row(
                          {.out = &m_typeHeader,
                           .paddingH = Style::spaceXs,
                           .minWidth = files::DetailsColumns::typeWidth + 2 * Style::spaceXs,
                           .maxWidth = files::DetailsColumns::typeWidth + 2 * Style::spaceXs},
                          ui::label({.text = "Type", .fontSize = Style::fontSizeCaption})
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
              ui::label({.out = &m_status, .maxLines = 1, .flexGrow = 1.0F}),
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
        bool alignLeft = false
    ) {
      cancelDrag();
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
    Button* m_modifiedHeader = nullptr;
    Flex* m_typeHeader = nullptr;
    files::DetailsColumns m_detailsColumns;
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
