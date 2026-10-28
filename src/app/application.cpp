#include "app/application.h"

#include "app/deferred_call_poll_source.h"
#include "app/glib_poll_source.h"
#include "app/main_loop.h"
#include "app/timer_poll_source.h"
#include "browser/browser_controller.h"
#include "browser/file_view.h"
#include "config/settings.h"
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
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "wayland/popup_surface.h"
#include "wayland/text_input_service.h"
#include "wayland/toplevel_surface.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <array>
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
    Window(
        WaylandConnection& connection, RenderContext& renderer, TextInputService& textInput,
        const files::BrowserSettings& settings
    )
        : m_connection(connection), m_renderer(renderer), m_textInput(textInput), m_surface(connection),
          m_browser(settings.sort, settings.showHidden),
          m_listAdapter(
              m_browser, files::ViewMode::LIST, [this] { return m_connection.input().keyboardModifiers(); },
              [this] { return m_list != nullptr && m_list->focusArea()->focused(); }
          ),
          m_gridAdapter(
              m_browser, files::ViewMode::GRID, [this] { return m_connection.input().keyboardModifiers(); },
              [this] { return m_list != nullptr && m_list->focusArea()->focused(); }
          ),
          m_viewMode(settings.viewMode) {
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
    }

    ~Window() {
      m_browser.setChangedCallback({});
      closeMenu();
      m_dispatcher.setSceneRoot(nullptr);
      m_surface.setSceneRoot(nullptr);
      m_root.reset();
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
      if (initialLocation) {
        (void)m_browser.openText(*initialLocation);
      } else {
        m_browser.home();
      }
      return true;
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
          m_location->selectAll();
          return;
        }
        if (event.sym == XKB_KEY_k && event.modifiers == KeyMod::Ctrl && m_filterInput != nullptr) {
          openFilter();
          return;
        }
        if (m_filterOpen
            && event.modifiers == 0
            && KeySymbol::isEscape(event.sym)
            && m_dispatcher.focusedArea() != m_location->inputArea()) {
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
      m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    }

  private:
    void openFilter() {
      m_filterOpen = true;
      m_focusFilter = true;
      m_browserDirty = true;
      m_surface.requestUpdate();
    }

    void changeNameFilter(const std::string& text) {
      if (text == m_browser.model().filter().text) {
        return;
      }
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      m_viewAnchor.reset();
      m_list->scrollView().requestScrollToOffset(0);
      m_browser.setNameFilter(text);
    }

    void closeFilter() {
      m_filterOpen = false;
      m_focusFilter = false;
      changeNameFilter({});
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
      m_browserDirty = true;
      m_surface.requestUpdate();
    }

    void applyViewMode() {
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      const bool grid = m_viewMode == files::ViewMode::GRID;
      m_list->setAdapter(grid ? &m_gridAdapter : &m_listAdapter);
      m_list->setColumns(grid ? 0 : 1);
      m_list->setMinCellWidth(132.0F);
      m_list->setCellHeight(grid ? 144.0F : 36.0F);
      m_list->setColumnGap(grid ? Style::spaceSm : 0.0F);
      m_list->setRowGap(grid ? Style::spaceSm : 2.0F);
      m_appliedViewMode = m_viewMode;
    }

    bool listKey(const KeyboardEvent& event) {
      auto& model = m_browser.model();
      if (event.sym == XKB_KEY_a && event.modifiers == KeyMod::Ctrl) {
        model.selectAll();
        return true;
      }
      if (event.modifiers == 0 && KeySymbol::isEscape(event.sym)) {
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
        m_filterOpen = false;
        m_focusFilter = false;
        if (m_dispatcher.focusedArea() == m_filterInput->inputArea()) {
          m_dispatcher.setFocus(m_list->focusArea());
        }
        m_displayedLocation = uri;
        m_location->setValue(uri);
        m_locationInvalid = false;
        m_listAdapter.onPointerCancel();
        m_gridAdapter.onPointerCancel();
        m_viewAnchor.reset();
        m_list->scrollView().requestScrollToOffset(0);
      }
      m_location->setInvalid(m_locationInvalid);
      m_filterBar->setVisible(m_filterOpen);
      m_filterBar->setParticipatesInLayout(m_filterOpen);
      m_filterButton->setSelected(m_filterOpen);
      if (m_filterInput->value() != model.filter().text) {
        m_filterInput->setValue(model.filter().text);
      }
      if (m_focusFilter) {
        m_focusFilter = false;
        m_dispatcher.setFocus(m_filterInput->inputArea());
        m_filterInput->selectAll();
      }
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
      m_sortButton->setTooltip(
          std::format("Sort: {}, {}{}", field, direction, sort.directoriesFirst ? " · Folders first" : "")
      );
      const bool grid = m_viewMode == files::ViewMode::GRID;
      m_viewSwitch->setSelectedIndex(grid ? 0 : 1);
      m_listHeader->setVisible(!grid);
      m_listHeader->setParticipatesInLayout(!grid);
      m_back->setEnabled(m_browser.canBack());
      m_forward->setEnabled(m_browser.canForward());
      m_up->setEnabled(m_browser.canUp());
      m_hidden->setSelected(model.filter().showHidden);
      const char* home = std::getenv("HOME");
      m_homePlace->setSelected(
          home != nullptr && g_path_is_absolute(home) && snapshot.location == files::Location::fromPath(home)
      );
      m_rootPlace->setSelected(snapshot.location == files::Location::fromPath("/"));
      const bool loading = snapshot.status == files::DirectoryStatus::LOADING;
      m_refresh->setGlyph(loading ? "square" : "refresh");
      m_refresh->setTooltip(loading ? "Stop loading" : "Refresh");
      std::string status;
      if (!m_browser.message().empty()) {
        status = m_browser.message();
      } else if (snapshot.error) {
        status = snapshot.error->message;
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
        status = model.filter().text.empty()
            ? std::format("{} items", model.rows().size())
            : std::format("{} of {} items", model.rows().size(), snapshot.entries.size() - hidden);
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
              ? status
              : snapshot.entries.empty()     ? "This folder is empty."
              : !model.filter().text.empty() ? "No matching items."
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
        syncBrowser();
        layout = true;
      }
      if (layout) {
        UiPhaseScope phase(UiPhase::Layout);
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
          ),
          ui::input(
              {.out = &m_location,
               .placeholder = "Location",
               .flexGrow = 1.0F,
               .onSubmit =
                   [this](const std::string& text) {
                     m_locationInvalid = !m_browser.openText(text);
                     if (!m_locationInvalid) {
                       m_dispatcher.setFocus(m_list->focusArea());
                     }
                   }}
          ),
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
              {.out = &m_filterButton,
               .glyph = "filter",
               .variant = ButtonVariant::Ghost,
               .tooltip = "Filter this folder (Ctrl+K)",
               .onClick =
                   [this] {
                     if (m_filterOpen) {
                       closeFilter();
                     } else {
                       openFilter();
                     }
                   }}
          ),
          ui::button(
              {.out = &m_sortButton,
               .glyph = "arrows-sort",
               .variant = ButtonVariant::Ghost,
               .onClick = [this] { openMenu(true); }}
          ),
          ui::button(
              {.out = &m_hidden,
               .glyph = "eye",
               .variant = ButtonVariant::Ghost,
               .tooltip = "Show hidden files (Ctrl+H)",
               .onClick = [this] { m_browser.toggleHidden(); }}
          ),
          ui::button(
              {.out = &m_menuButton, .glyph = "dots-vertical", .variant = ButtonVariant::Ghost, .onClick = [this] {
                 openMenu();
               }}
          )
      );

      auto filterBar = ui::row(
          {.out = &m_filterBar,
           .align = FlexAlign::Center,
           .gap = Style::spaceSm,
           .paddingV = Style::spaceXs,
           .paddingH = Style::spaceSm,
           .visible = false,
           .participatesInLayout = false},
          ui::label({.text = "Filter this folder", .fontSize = Style::fontSizeCaption}),
          ui::input(
              {.out = &m_filterInput,
               .placeholder = "Filter names…",
               .clearButtonEnabled = true,
               .flexGrow = 1.0F,
               .onChange = [this](const std::string& text) { changeNameFilter(text); },
               .onSubmit =
                   [this](const std::string&) {
                     if (!m_browser.model().rows().empty()) {
                       m_dispatcher.setFocus(m_list->focusArea());
                     }
                   }}
          ),
          ui::button(
              {.glyph = "x", .variant = ButtonVariant::Ghost, .tooltip = "Close filter (Escape)", .onClick = [this] {
                 closeFilter();
               }}
          )
      );

      auto places = ui::column(
          {.gap = Style::spaceXs,
           .padding = Style::spaceMd,
           .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
           .minWidth = 206.0F}
      );
      places->addChild(
          ui::button(
              {.out = &m_homePlace,
               .text = "Home",
               .glyph = "home",
               .contentAlign = ButtonContentAlign::Start,
               .variant = ButtonVariant::Ghost,
               .minWidth = 174.0F,
               .onClick = [this] { m_browser.home(); }}
          )
      );
      const std::array<std::pair<const char*, const char*>, 4> entries{{
          {"clock", "Recent"},
          {"star", "Starred"},
          {"network", "Network"},
          {"trash", "Trash"},
      }};
      for (const auto& [glyph, label] : entries) {
        places->addChild(
            ui::button(
                {.text = label,
                 .glyph = glyph,
                 .enabled = false,
                 .contentAlign = ButtonContentAlign::Start,
                 .variant = ButtonVariant::Ghost,
                 .minWidth = 174.0F}
            )
        );
      }
      places->addChild(ui::separator({}));
      places->addChild(
          ui::button(
              {.out = &m_rootPlace,
               .text = "File System",
               .glyph = "device-desktop",
               .contentAlign = ButtonContentAlign::Start,
               .variant = ButtonVariant::Ghost,
               .minWidth = 174.0F,
               .onClick = [this] { m_browser.open(files::Location::fromPath("/")); }}
          )
      );
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
          std::move(header), std::move(toolbar), std::move(filterBar),
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
      m_menuDispatcher.setSceneRoot(nullptr);
      if (m_menuSurface != nullptr) {
        m_menuSurface->setSceneRoot(nullptr);
      }
      m_menuRoot.reset();
      m_menu = nullptr;
      m_menuSurface.reset();
    }
    void openMenu(bool sorting = false) {
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
      std::vector<ContextMenuControlEntry> entries;
      if (sorting) {
        const auto& sort = m_browser.model().sort();
        entries = {
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
        };
      } else {
        entries = {
            {.id = 1, .label = "Minimize"},
            {.id = 2, .label = "Maximize / Restore"},
            {.id = 3, .label = "Fullscreen"},
            {.id = 4, .label = "Close"}
        };
      }
      const float menuHeight = ContextMenuControl::preferredHeight(entries, entries.size(), 1.0F);
      const float menuWidth = sorting ? 264.0F : 240.0F;
      menu->setEntries(std::move(entries));
      menu->setMenuWidth(menuWidth);
      menu->setOnActivate([this, sorting](const ContextMenuControlEntry& entry) {
        dismissMenu();
        if (sorting) {
          auto options = m_browser.model().sort();
          if (entry.id >= 1 && entry.id <= 3) {
            const auto field = entry.id == 1 ? files::SortField::NAME
                : entry.id == 2              ? files::SortField::SIZE
                                             : files::SortField::MODIFIED;
            if (field != options.field) {
              options.field = field;
              options.descending = false;
            }
          } else if (entry.id == 4 || entry.id == 5) {
            options.descending = entry.id == 5;
          } else if (entry.id == 6) {
            options.directoriesFirst = !options.directoriesFirst;
          }
          changeSort(options);
          return;
        }
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
      const auto* anchor = sorting ? m_sortButton : m_menuButton;
      float anchorX = 0, anchorY = 0;
      Node::absolutePosition(anchor, anchorX, anchorY);
      PopupSurfaceConfig config{
          .anchorX = static_cast<int>(anchorX),
          .anchorY = static_cast<int>(anchorY),
          .anchorWidth = static_cast<int>(anchor->width()),
          .anchorHeight = static_cast<int>(anchor->height()),
          .width = static_cast<std::uint32_t>(menuWidth + Style::spaceSm * 2),
          .height = static_cast<std::uint32_t>(menuHeight + Style::spaceSm * 2),
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
    files::BrowserController m_browser;
    files::FileViewAdapter m_listAdapter;
    files::FileViewAdapter m_gridAdapter;
    files::ViewMode m_viewMode;
    std::optional<files::ViewMode> m_appliedViewMode;
    std::optional<files::EntryId> m_viewAnchor;
    std::unique_ptr<Node> m_root;
    InputDispatcher m_dispatcher;
    Flex* m_titlebar = nullptr;
    Input* m_location = nullptr;
    Input* m_filterInput = nullptr;
    Flex* m_filterBar = nullptr;
    Button* m_filterButton = nullptr;
    bool m_filterOpen = false;
    bool m_focusFilter = false;
    Label* m_status = nullptr;
    Label* m_notice = nullptr;
    VirtualGridView* m_list = nullptr;
    Flex* m_listHeader = nullptr;
    Segmented* m_viewSwitch = nullptr;
    Button* m_nameHeader = nullptr;
    Button* m_sizeHeader = nullptr;
    Button* m_sortButton = nullptr;
    Button* m_menuButton = nullptr;
    Button* m_back = nullptr;
    Button* m_forward = nullptr;
    Button* m_up = nullptr;
    Button* m_refresh = nullptr;
    Button* m_hidden = nullptr;
    Button* m_homePlace = nullptr;
    Button* m_rootPlace = nullptr;
    std::string m_displayedLocation;
    bool m_browserDirty = true;
    bool m_locationInvalid = false;
    std::unique_ptr<PopupSurface> m_menuSurface;
    std::unique_ptr<Flex> m_menuRoot;
    ContextMenuControl* m_menu = nullptr;
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
    Window window(connection, renderer, textInput, config.settings().browser);
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
