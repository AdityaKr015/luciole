#include "app/application.h"

#include "app/deferred_call_poll_source.h"
#include "app/glib_poll_source.h"
#include "app/library_setup.h"
#include "app/main_loop.h"
#include "app/timer_poll_source.h"
#include "browser/action_registry.h"
#include "browser/browser_controller.h"
#include "browser/details_columns.h"
#include "browser/file_view.h"
#include "browser/location_presentation.h"
#include "browser/path_completion.h"
#include "browser/places_sidebar.h"
#include "browser/preview_layout.h"
#include "browser/preview_service.h"
#include "browser/selection_tools.h"
#include "browser/tab_workspace.h"
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
#include "platform/text_clipboard.h"
#include "render/animation/animation_manager.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "system/icon_theme_poll_source.h"
#include "ui/builders.h"
#include "ui/controls/archive_view.h"
#include "ui/controls/batch_rename_dialog.h"
#include "ui/controls/browser_settings_view.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/file_preview_view.h"
#include "ui/controls/input.h"
#include "ui/controls/link_dialog.h"
#include "ui/controls/miller_view.h"
#include "ui/controls/name_editor.h"
#include "ui/controls/permission_editor.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/select_popup_context.h"
#include "ui/controls/tooltip_presenter.h"
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
#include <map>
#include <memory>
#include <print>
#include <string>
#include <unordered_set>
#include <utility>

namespace {
  constexpr float kWorkspaceInset = Style::spaceSm;
  constexpr float kWorkspaceRadius = Style::radiusXl;
  constexpr float kMinimumPaneWidth = 160.0F;
  constexpr float kMinimumPreviewWidth = 208.0F;
  constexpr float kMinimumFreeSpacePaneWidth = 320.0F;

  volatile std::sig_atomic_t g_stopRequested = 0;
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

  using Action = files::BrowserAction;

  enum class WorkspaceCommand {
    ClosePane,
    NewTab,
    Duplicate,
    Close,
    Reopen,
    Split,
    SwitchArea,
    Swap,
    NewWindow,
    Detach,
    MoveWindow,
    OpenOther,
    CopyOther,
    MoveOther
  };

  class BrowserPage : public TextInputClient, public SelectPopupContext {
  public:
    BrowserPage(
        WaylandConnection& connection, RenderContext& renderer, TextInputService& textInput,
        const files::BrowserSettings& settings, files::WaylandTextClipboard& clipboard, files::WindowSurface& surface,
        AnimationManager& animations, files::PinService& pins, files::FileIconCache& icons,
        files::ViewPreferenceWriter& preferences
    )
        : m_connection(connection), m_renderer(renderer), m_textInput(textInput), m_animations(animations),
          m_surface(&surface), m_browser(settings.sort, settings.showHidden), m_icons(icons),
          m_listAdapter(
              m_browser, m_icons, m_thumbnails, files::ViewMode::List,
              [this] { return m_connection.keyboardModifiers(); },
              [this] { return m_fileKeyboardFocus && m_list != nullptr && m_list->focusArea()->focused(); }
          ),
          m_gridAdapter(
              m_browser, m_icons, m_thumbnails, files::ViewMode::Grid,
              [this] { return m_connection.keyboardModifiers(); },
              [this] { return m_fileKeyboardFocus && m_list != nullptr && m_list->focusArea()->focused(); }
          ),
          m_preferences(preferences), m_savedSettings(settings), m_pins(pins), m_viewMode(settings.viewMode),
          m_clipboard(clipboard) {
      if (m_preferences.settings()) {
        m_savedSettings = m_preferences.settings()->browser;
      }
      m_gridZoom = static_cast<std::size_t>(
          std::ranges::find(files::kGridIconSizes, settings.gridIconSize) - files::kGridIconSizes.begin()
      );
      browser().setViewDefaults(settings.viewMode, settings.gridIconSize);
      m_thumbnails.setOptions(settings.thumbnails);
      m_preview.setChangedCallback([this] {
        m_surface->requestLayout();
        if (m_menuSurface) {
          m_menuSurface->requestLayout();
        }
      });
      m_thumbnails.setChangedCallback([this] {
        if (m_thumbnailClearing != m_thumbnails.clearing() || m_thumbnailError != m_thumbnails.error()) {
          m_thumbnailClearing = m_thumbnails.clearing();
          m_thumbnailError = m_thumbnails.error();
          m_settingsDirty = true;
        }
        m_surface->requestLayout();
      });
      m_browser.setCaptureViewport([this] {
        files::BrowseViewport viewport{.mode = m_viewMode, .iconSize = static_cast<int>(gridIconSize())};
        if (m_list) {
          viewport.vertical = m_list->scrollView().scrollOffset();
          viewport.horizontal = m_detailsOffset;
          viewport.withinRow = m_list->firstVisibleOffset();
          if (const auto first = m_list->firstVisibleIndex(); first && *first < browser().model().rows().size()) {
            viewport.first = browser().model().rows()[*first];
          }
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
      m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
        m_connection.extension<files::DesktopIntegration>().setCursorShape(serial, shape);
      });
      m_dispatcher.setFocusChangeCallback([this](InputArea*, InputArea* focused) {
        if (m_settingsView) {
          m_settingsView->reveal(focused);
          m_surface->requestLayout();
        }
        if (m_list != nullptr && focused != m_list->focusArea()) {
          clearTypeahead();
        }
        if (m_nameHeader == nullptr || m_viewMode != files::ViewMode::List) {
          return;
        }
        for (std::size_t index = 0; index < 4; ++index) {
          if (widthHeader(index)->inputArea() == focused) {
            m_revealDetailsHeader = widthHeader(index);
            m_surface->requestLayout();
          }
        }
      });
      m_browser.setChangedCallback([this] {
        m_browserDirty = true;
        m_surface->requestUpdate();
      });
      m_browser.setFileActivatedCallback([this](const files::Location& location) { m_launcher.open(location); });
      m_miller.defaultWidth = settings.millerColumnWidth;
      m_miller.setChangedCallback([this] {
        m_browserDirty = true;
        m_surface->requestUpdate();
      });
      m_miller.setFileActivatedCallback([this](const files::Location& target) { m_launcher.open(target); });
      m_launcher.setChangedCallback([this](const std::string& error) {
        m_actionError = error.empty() ? "" : std::format("Could not open the file: {}", error);
        if (m_aboutMessage) {
          m_aboutMessage->setText(error.empty() ? "" : std::format("Could not open the link: {}", error));
          m_aboutMessage->setVisible(!error.empty());
          m_aboutMessage->setParticipatesInLayout(!error.empty());
          m_menuSurface->requestLayout();
        }
        m_browserDirty = true;
        m_surface->requestUpdate();
      });
      m_archives.setChangedCallback([this] {
        updateOperationProgress();
        m_archiveDirty = true;
        if (auto file = m_archives.takeOpenedFile()) {
          m_launcher.open(*file);
        }
        if (!m_archives.busy()) {
          m_actionError = m_archives.error();
          if (!m_archives.results().empty()) {
            if (m_actionError.empty()) {
              m_actionError = "Archive operation complete.";
            }
            browser().refresh();
          }
        }
        m_browserDirty = true;
        m_surface->requestUpdate();
      });
      m_transfers.setChangedCallback([this] {
        updateOperationProgress();
        if (!m_archives.busy() && !m_transfers.busy()) {
          const auto& results = m_transfers.results();
          const auto done = std::ranges::count_if(results, [](const auto& item) {
            return item.outcome == files::TransferOutcome::Moved || item.outcome == files::TransferOutcome::Copied;
          });
          m_actionError = std::format(
              "{} item{} {}", done, done == 1 ? "" : "s",
              m_transfers.action() == files::TransferAction::Copy ? "copied" : "moved"
          );
          if (const auto error = std::ranges::find_if(results, [](const auto& item) { return !item.error.empty(); });
              error != results.end()) {
            const files::GString raw(g_file_get_basename(error->source.file().get()));
            const files::GString name(g_filename_display_name(raw ? raw.get() : "Item"));
            m_actionError += std::format(" · {}: {}", name.get(), error->error);
          }
          if (const auto selection = std::exchange(m_clipboardTransfer, std::nullopt)) {
            m_clipboard.finishMove(*selection, results);
          }
          if (auto finish = std::exchange(m_dropCompletion, {})) {
            finish(!results.empty() && std::ranges::all_of(results, [](const auto& item) {
              return item.outcome == files::TransferOutcome::Moved || item.outcome == files::TransferOutcome::Copied;
            }));
          }
          browser().refresh();
        }
        m_browserDirty = true;
        m_surface->requestUpdate();
      });
      m_emptyTrash.setChangedCallback([this] {
        updateOperationProgress();
        m_emptyTrashDirty = true;
        m_browserDirty = true;
        m_surface->requestUpdate();
      });
      m_restore.setChangedCallback([this] {
        updateOperationProgress();
        if (!m_restore.busy()) {
          const auto& results = m_restore.results();
          const auto count = [&](files::RestoreOutcome outcome) {
            return std::ranges::count(results, outcome, &files::RestoreResult::outcome);
          };
          const auto restored = count(files::RestoreOutcome::Restored);
          m_actionError = std::format("{} item{} restored", restored, restored == 1 ? "" : "s");
          for (const auto& [outcome, label] :
               {std::pair{files::RestoreOutcome::Skipped, "skipped"},
                std::pair{files::RestoreOutcome::Failed, "failed"},
                std::pair{files::RestoreOutcome::Cancelled, "cancelled"},
                std::pair{files::RestoreOutcome::Unconfirmed, "could not be confirmed"}}) {
            if (const auto amount = count(outcome); amount != 0) {
              m_actionError += std::format(" · {} {}", amount, label);
            }
          }
          const auto error = std::ranges::find_if(results, [](const auto& item) { return !item.error.empty(); });
          if (error != results.end()) {
            m_actionError += std::format(" · {}: {}", error->displayName, error->error);
          }
          browser().refresh();
        }
        m_browserDirty = true;
        m_surface->requestUpdate();
      });
      m_trash.setChangedCallback([this] {
        updateOperationProgress();
        m_trashDirty = true;
        m_browserDirty = true;
        m_surface->requestUpdate();
      });
      m_creator.setChangedCallback([this] {
        m_creationDirty = true;
        m_browserDirty = true;
        m_surface->requestUpdate();
      });
      m_renamer.setChangedCallback([this] {
        m_renameDirty = true;
        m_browserDirty = true;
        m_surface->requestUpdate();
      });
      m_permissions.setChangedCallback([this] {
        m_permissionDirty = true;
        m_browserDirty = true;
        m_surface->requestUpdate();
      });
      m_batchRenamer.setChangedCallback([this] {
        updateOperationProgress();
        m_batchRenameDirty = true;
        m_browserDirty = true;
        m_surface->requestUpdate();
      });
      for (auto* adapter : {&m_listAdapter, &m_gridAdapter}) {
        adapter->setCutPredicate([this](const files::Location& location) { return m_clipboard.isCut(location); });
        adapter->setActivationMode(m_activationMode);
        adapter->setActivateCallback([this](files::EntryId id) {
          auto request = actionRequest(false);
          if (const auto* entry = browser().model().entry(id)) {
            request.targets.push_back({id, entry->location});
          }
          dispatchAction(Action::Open, request);
        });
        adapter->setContextCallback([this](std::optional<files::EntryId> id, float x, float y) {
          openFileMenu(id, x, y);
        });
        adapter->setDragCallback(
            [this](const std::vector<files::EntryId>& ids) { startFileDrag(ids); },
            [this] {
              if (m_drag && !m_drag->sourcePin && !m_systemDrag) {
                cancelDrag();
              }
            }
        );
      }
    }

    files::BrowserController& browser() { return m_miller.enabled() ? m_miller.active() : m_browser; }
    const files::BrowserController& browser() const { return m_miller.enabled() ? m_miller.active() : m_browser; }
    void focusMiller(std::size_t index, bool reveal = true) {
      if (!m_millerView || index >= m_miller.columns().size()) {
        return;
      }
      const bool changed = m_miller.focus(index);
      if (auto* view = m_millerView->focusedView()) {
        m_list = view;
      } else {
        m_millerFocusPending = true;
      }
      if (changed) {
        clearTypeahead();
        m_visitSerial = 0;
        m_pendingHistoryViewport.reset();
        m_keyboardReveal.reset();
        m_thumbnailContext = 0;
        m_browserDirty = true;
        if (reveal) {
          m_millerView->revealFocused();
        }
        m_surface->requestUpdate();
      }
    }

    ~BrowserPage() override {
      m_dragScroll.stop();
      m_externalScroll.stop();
      m_operationProgress.stop();
      m_archives.setChangedCallback({});
      m_transfers.setChangedCallback({});
      if (m_clipboardTransfer) {
        m_clipboard.finishMove(*m_clipboardTransfer, m_transfers.results());
      }
      if (m_dropCompletion) {
        m_dropCompletion(false);
      }
      m_trash.setChangedCallback({});
      m_emptyTrash.setChangedCallback({});
      m_restore.setChangedCallback({});
      m_renamer.setChangedCallback({});
      m_batchRenamer.setChangedCallback({});
      m_permissions.setChangedCallback({});
      m_creator.setChangedCallback({});
      m_browser.setChangedCallback({});
      m_miller.setChangedCallback({});
      m_miller.setFileActivatedCallback({});
      m_browser.setFileActivatedCallback({});
      m_launcher.setChangedCallback({});
      m_thumbnails.setChangedCallback({});
      m_preview.setChangedCallback({});
      m_preview.clear();
      closeMenu();
      closeSettings();
      closeArchive();
      m_dispatcher.setSceneRoot(nullptr);
      unmount();
      m_root = nullptr;
      m_detachedRoot.reset();
    }

    void iconThemeChanged() {
      m_browserDirty = true;
      m_surface->requestUpdate();
    }

    bool initialize(const std::optional<std::string>& initialLocation) {
      if (initialLocation) {
        (void)browser().openText(*initialLocation);
      } else {
        browser().home();
      }
      return true;
    }

    bool renameBusy() const { return m_renamer.busy() || m_batchRenamer.busy() || m_permissions.busy(); }
    bool busy() const {
      return m_thumbnails.clearing()
          || m_preview.busy()
          || m_preferences.busy()
          || m_pins.busy()
          || m_launcher.busy()
          || m_archives.busy()
          || m_transfers.busy()
          || m_restore.busy()
          || m_trash.busy()
          || m_emptyTrash.busy()
          || renameBusy()
          || m_creator.busy()
          || m_pendingFilePaste;
    }
    void shutdown() {
      m_preview.clear();
      m_pendingFilePaste = false;
      m_launcher.cancelApplications();
      m_archives.shutdown();
      m_transfers.cancel();
      m_emptyTrash.cancel();
      m_restore.cancel();
      m_trash.cancel();
      m_renamer.cancel();
      m_batchRenamer.cancel();
      m_permissions.cancel();
      m_creator.cancel();
    }

    files::BrowserSettings options() const {
      auto value = m_savedSettings;
      value.viewMode = m_viewMode;
      value.gridIconSize = static_cast<int>(gridIconSize());
      value.columns = m_columnVisibility;
      value.columnOrder = m_columnOrder;
      value.columnWidths = m_columnWidths;
      value.sort = browser().model().sort();
      value.showHidden = browser().model().filter().showHidden;
      return value;
    }
    std::optional<files::Location> location() const { return browser().model().snapshot().location; }
    std::string title() const {
      if (!location()) {
        return "Files";
      }
      const files::GString raw(g_file_get_basename(location()->file().get()));
      const files::GString text(g_filename_display_name(raw ? raw.get() : "Files"));
      return text.get();
    }
    void preferencesChanged(const std::string& error) {
      m_preferenceError = error;
      m_pendingSettings = m_preferences.settings();
      m_settingsDirty = true;
      m_browserDirty = true;
      m_surface->requestUpdate();
    }
    void placesChanged() {
      m_placesDirty = true;
      m_browserDirty = true;
      m_surface->requestUpdate();
    }
    void clipboardChanged() {
      m_browserDirty = true;
      m_surface->requestUpdate();
    }
    void clearExternalDrop() {
      m_externalDraggingHere = false;
      m_refreshExternalDrop = false;
      m_externalScroll.stop();
      m_externalScrollDelta = 0;
      if (m_externalDropTarget || m_externalPinSlot) {
        m_externalDropTarget.reset();
        m_externalPinSlot.reset();
        m_surface->requestLayout();
      }
    }
    const std::vector<files::Bookmark>* draggedFolders() const {
      return m_systemDrag && m_drag && m_drag->foldersOnly ? &m_drag->bookmarks : nullptr;
    }
    files::FileDropTarget externalDrop(float x, float y, const std::vector<files::Bookmark>* pins) {
      clearExternalDrop();
      if (!m_visible || overlay() || popup() || actionContext().operationsBusy) {
        return {};
      }
      m_pointerX = x;
      m_pointerY = y;

      m_externalDraggingHere = true;
      updateExternalScroll();
      // External offers are decoded only after a drop. Native worker validation
      // still checks self/descendant/alias targets before any filesystem change.
      if (pins && m_showSidebar && m_pins.editable() && !m_places->folderAt(x, y)) {
        if (const auto slot = m_places->dropSlot(x, y)) {
          m_externalPinSlot = slot;
          updateExternalScroll();
          m_surface->requestLayout();
          const std::weak_ptr<int> lifetime = m_lifetime;
          return {
              .receive =
                  [this, lifetime, bookmarks = *pins,
                   slot =
                       *slot](const files::FilePayload&, files::TransferAction, const files::DropCompletion& finish) {
                    if (lifetime.expired() || !m_visible || !m_pins.editable()) {
                      finish(false);
                      return;
                    }
                    finish(m_pins.add(bookmarks, slot));
                  },
              .preferred = files::TransferAction::Copy
          };
        }
      }
      m_externalHover = true;
      const auto target = dropTarget();
      m_externalHover = false;
      if (!target) {
        return {};
      }
      return externalDestination(target->location, target->rect);
    }
    files::FileDropTarget externalDestination(const files::Location& destination, LayoutRect rect) {
      if (!m_visible || overlay() || popup() || actionContext().operationsBusy || !destination.nativePath()) {
        return {};
      }
      m_externalDropTarget = DropTarget{destination, rect};
      updateExternalScroll();
      m_surface->requestLayout();
      const auto context = browser().contextSerial();
      const std::weak_ptr<int> lifetime = m_lifetime;
      return {
          .receive =
              [this, lifetime, context,
               destination](files::FilePayload payload, files::TransferAction action, files::DropCompletion finish) {
                if (lifetime.expired()
                    || context != browser().contextSerial()
                    || !m_visible
                    || overlay()
                    || actionContext().operationsBusy) {
                  finish(false);
                  return;
                }
                m_dropCompletion = std::move(finish);
                if (!startPayloadTransfer(std::move(payload), destination, action)) {
                  std::exchange(m_dropCompletion, {})(false);
                }
              },
          .preferred = (m_connection.keyboardModifiers() & KeyMod::Ctrl) != 0 ? files::TransferAction::Copy
                                                                              : files::TransferAction::Move
      };
    }
    void iconsReady() {
      m_surface->requestLayout();
      if (m_menuSurface && !m_menu) {
        m_menuSurface->requestLayout();
      }
    }
    void configurePlacesCallbacks() {
      m_places->setOpenCallback([this](const files::Location& location) {
        m_actionError.clear();
        browser().open(location);
        m_dispatcher.setFocus(m_list->focusArea());
      });
      m_places->setContextCallback([this](const files::Bookmark& bookmark, float x, float y) {
        openPlaceMenu(bookmark, x, y);
      });
      m_places->setDragCallback([this](const files::Bookmark& bookmark) { startPlaceDrag(bookmark); });
      m_places->setRenameCallback([this](const files::Bookmark& bookmark) { openPlaceRename(bookmark); });
    }
    void reusePlacesFrom(BrowserPage& previous) {
      if (&previous == this || !m_sidebarBundle || !previous.m_sidebarBundle) {
        return;
      }
      auto ready = previous.m_sidebarBundle->removeChild(previous.m_places);
      auto placeholder = m_sidebarBundle->removeChild(m_places);
      std::swap(m_places, previous.m_places);
      m_sidebarBundle->insertChildAt(0, std::move(ready));
      previous.m_sidebarBundle->insertChildAt(0, std::move(placeholder));
      m_placesDirty = previous.m_placesDirty;
      previous.m_placesDirty = true;
      configurePlacesCallbacks();
      previous.configurePlacesCallbacks();
    }
    void workspaceError(const std::string& message) { actionError(message); }
    void setWorkspaceId(files::TabId id) { m_workspaceId = id; }
    bool owns(const Node* node) const {
      for (; node; node = node->parent()) {
        if (node == m_root || node == m_toolbar || node == m_sidebarBundle) {
          return true;
        }
      }
      return false;
    }
    void focusArea(InputArea* area) { m_dispatcher.setFocus(area); }
    void leavePointer() { m_dispatcher.pointerLeave(); }
    InputArea* cycleFocus(bool reverse) {
      (void)m_dispatcher.cycleTabFocus(reverse);
      return m_dispatcher.focusedArea();
    }
    bool completeLocationOnTab(std::uint32_t modifiers) {
      return m_editingLocation
          && m_location != nullptr
          && m_dispatcher.focusedArea() == m_location->inputArea()
          && completionKey(XKB_KEY_Tab, modifiers);
    }
    void nativeFocus() {
      if (m_active) {
        m_dispatcher.setTextInputContext(m_surface->wlSurface(), &m_textInput);
      }
    }
    void stampFocusKeys(Node& node) {
      if (auto* area = dynamic_cast<InputArea*>(&node); area && area->tabFocusKey().empty()) {
        area->setTabFocusKey(std::format("tab:{}:{}", m_workspaceId, ++m_nextFocusKey));
      }
      for (const auto& child : node.children()) {
        stampFocusKeys(*child);
      }
    }
    void duplicateFrom(BrowserPage& source) { browser().duplicateFrom(source.browser()); }
    void open(const files::Location& location) { browser().open(location); }
    void setWorkspaceRequest(std::function<void(WorkspaceCommand, std::optional<files::Location>)> callback) {
      m_workspaceRequest = std::move(callback);
    }
    void setClearThumbnails(std::function<void()> callback) { m_clearThumbnails = std::move(callback); }
    void clearThumbnails() { m_thumbnails.clearCache(); }
    bool clearingThumbnails() const { return m_thumbnails.clearing(); }
    void pauseThumbnails(bool pause) {
      m_cachePaused = pause;
      auto options = m_savedSettings.thumbnails;
      if (pause) {
        options.policy = files::ThumbnailPolicy::Never;
      }
      m_thumbnails.setOptions(options);
      m_surface->requestLayout();
    }
    void configurationChanged() {
      clearExternalDrop();
      m_blankLocationClick.reset();
      if (m_resizingSidebar || m_columnDrag || m_columnResize || (m_millerView && m_millerView->resizing())) {
        m_dispatcher.cancelPointerCapture();
      }
    }
    void setHasOther(std::function<bool()> callback) { m_hasOther = std::move(callback); }
    void setPopupAvailable(std::function<bool()> callback) { m_popupAvailable = std::move(callback); }
    void setForeignDrop(std::function<std::optional<std::pair<files::Location, LayoutRect>>(float, float)> callback) {
      m_foreignDrop = std::move(callback);
    }
    bool popupOwns(wl_surface* surface) const {
      return (m_menuSurface && m_menuSurface->wlSurface() == surface)
          || (m_documentSurface && m_documentSurface->wlSurface() == surface);
    }
    bool popup() const { return m_menuSurface != nullptr; }
    bool overlay() const { return m_settingsView || m_archiveView; }
    bool previewVisible() const { return m_savedSettings.showPreview && !overlay(); }
    int previewWidth() const { return m_savedSettings.previewWidth; }
    float previewAreaWidth() const { return m_previewBounds ? m_previewBounds->width : 0; }
    void savePreviewWidth(int width) {
      m_savedSettings.previewWidth = std::clamp(width, 208, 800);
      m_preferences.submit({{files::BrowserPreference::PreviewWidth, m_savedSettings.previewWidth}});
      m_surface->requestLayout();
    }
    void setPreviewBounds(std::optional<LayoutRect> bounds) { m_previewBounds = bounds; }
    void layoutNavigation(Renderer& renderer) {
      if (!m_toolbar->visible()) {
        return;
      }
      m_toolbar->arrange(renderer, {m_toolbar->x(), m_toolbar->y(), m_toolbar->width(), m_toolbar->height()});
      layoutBreadcrumbs(renderer);
      layoutCompletions(renderer);
      if (std::exchange(m_refreshExternalDrop, false)) {
        m_clipboard.refreshDrag();
      }
    }
    bool dragging() const { return m_drag.has_value(); }
    bool pointerCaptured() const { return m_dispatcher.pointerCaptured(); }
    bool contains(float x, float y) const { return m_body && m_body->containsScenePoint(x, y); }
    files::WindowSurface* boundSurface() const { return m_surface; }
    void rebind(files::WindowSurface& surface) {
      deactivate();
      m_surface = &surface;
    }
    void prepare(LayoutRect bounds, bool active, Node* host, bool roundLeft = true, bool roundRight = true) {
      m_pageBounds = bounds;
      if (!m_visible) {
        m_visible = true;
        m_appliedViewMode.reset();
        m_pendingHistoryViewport = m_inactiveViewport;
      }
      const bool split = m_hasOther && m_hasOther();
      m_paneHeader->setVisible(split);
      m_paneHeader->setParticipatesInLayout(split);
      m_paneTitle->setText(title());
      m_paneTitle->setColor(colorSpecFromRole(active ? ColorRole::Primary : ColorRole::OnSurfaceVariant));
      prepareFrame(true, true);
      stampFocusKeys(*m_root);
      stampFocusKeys(*m_toolbar);
      stampFocusKeys(*m_sidebarBundle);
      m_body->setBorder(
          colorSpecFromRole(active ? (split ? ColorRole::Primary : ColorRole::OutlineVariant) : ColorRole::Surface),
          split && active ? 2.0F : 1.0F
      );
      m_body->setRadii(
          {roundLeft ? kWorkspaceRadius : 0.0F, roundRight ? kWorkspaceRadius : 0.0F,
           roundRight ? kWorkspaceRadius : 0.0F, roundLeft ? kWorkspaceRadius : 0.0F}
      );
      m_body->setPadding(Style::radiusMd);
      if (active && !m_active) {
        m_active = true;
        m_dispatcher.setSceneRoot(host);
        m_dispatcher.setTextInputContext(m_surface->wlSurface(), &m_textInput);
        if (m_stashedFocus) {
          m_dispatcher.restoreTabFocus(*m_stashedFocus);
        }
        if (!m_dispatcher.focusedArea()) {
          m_dispatcher.setFocus(m_list->focusArea());
        }
      } else if (!active && m_active) {
        deactivate();
      }
      syncPreview();
      if (m_previewBounds && active && previewVisible()) {
        if (!m_previewView) {
          auto view = makePreviewView(false);
          m_previewView = view.get();
          view->setParticipatesInLayout(false);
          view->setZIndex(20);
          m_root->addChild(std::move(view));
        }
        m_previewView->setVisible(true);
        m_previewView->sync(
            m_preview.snapshot(), m_previewBounds->width, m_previewBounds->height,
            m_preview.snapshot().entry ? browser().model().typeLabel(m_preview.snapshot().entry->id) : ""
        );
        if (!m_preview.snapshot().image || !m_preview.snapshot().image->pixels) {
          m_previewView->clear(m_surface->renderTarget().renderer());
        }
        m_previewView->arrange(m_surface->renderTarget().renderer(), *m_previewBounds);
      } else if (m_previewView) {
        m_previewView->setVisible(false);
        m_previewView->clear(m_surface->renderTarget().renderer());
      }
    }
    void deactivate() {
      clearExternalDrop();
      m_preview.clear();
      if (m_previewView) {
        m_previewView->setVisible(false);
      }
      if (m_previewView && m_renderer.makeCurrent(m_surface->renderTarget())) {
        m_previewView->clear(m_surface->renderTarget().renderer());
      }
      m_active = false;
      if (m_root) {
        if (owns(m_dispatcher.focusedArea())) {
          m_stashedFocus = m_dispatcher.captureTabFocus();
        } else {
          m_stashedFocus.reset();
        }
        m_dispatcher.setFocus(nullptr);
      }
      m_activeTransferConflict.reset();
      closeMenu();
      clearTypeahead();
      cancelDrag();
      m_dispatcher.setSceneRoot(m_root);
      m_dispatcher.setTextInputContext(nullptr, nullptr);
    }
    void hide() {
      deactivate();
      if (m_visible && m_list) {
        m_inactiveViewport = files::BrowseViewport{
            m_viewMode,
            static_cast<int>(gridIconSize()),
            m_list->scrollView().scrollOffset(),
            m_detailsOffset,
            m_list->firstVisibleOffset(),
            m_list->firstVisibleIndex() && *m_list->firstVisibleIndex() < browser().model().rows().size()
                ? std::optional(browser().model().rows()[*m_list->firstVisibleIndex()])
                : std::nullopt
        };
        m_list->setAdapter(nullptr);
        m_millerView->suspend();
        m_thumbnails.invalidate();
        m_thumbnails.synchronizeSources({});
        m_thumbnailContext = 0;
        m_thumbnailRevision = 0;
      }
      m_visible = false;
    }
    void mount(Node& host) {
      if (m_detachedRoot) {
        host.addChild(std::move(m_detachedRoot));
      }
      if (m_detachedDragOverlay) {
        host.addChild(std::move(m_detachedDragOverlay));
      }
    }
    void unmount() {
      if (m_dragOverlay && m_dragOverlay->parent()) {
        m_detachedDragOverlay = m_dragOverlay->parent()->removeChild(m_dragOverlay);
      }
      if (m_root && m_root->parent()) {
        m_detachedRoot = m_root->parent()->removeChild(m_root);
      }
    }
    Node* root() const { return m_root; }
    std::unique_ptr<Node> takeToolbar() { return m_toolbar->parent()->removeChild(m_toolbar); }
    void returnToolbar(std::unique_ptr<Node> node) {
      if (node) {
        node->setVisible(false);
        m_root->addChild(std::move(node));
      }
    }
    void takeSidebar(Node& host) {
      if (m_detachedSidebar) {
        host.addChild(std::move(m_detachedSidebar));
      }
    }
    void returnSidebar() {
      if (m_sidebarBundle && m_sidebarBundle->parent()) {
        m_detachedSidebar = m_sidebarBundle->parent()->removeChild(m_sidebarBundle);
      }
    }
    Node* sidebar() const { return m_sidebarBundle; }
    void ensureScene() {
      if (!m_root) {
        UiPhaseScope phase(UiPhase::Layout);
        buildScene();
      }
    }

    float sidebarWidth() const {
      return m_showSidebar
          ? static_cast<float>(std::min(m_sidebarWidth, sidebarMaximumWidth())) + kSidebarHandleWidth + kWorkspaceInset
          : 0.0F;
    }
    void pumpJobs() {
      if (m_root) {
        syncTransferConflict();
        if (m_trashDirty) {
          m_trashDirty = false;
          syncTrash();
        }
        if (m_emptyTrashDirty) {
          m_emptyTrashDirty = false;
          syncEmptyTrash();
        }
      }
    }
    void
    workspaceMenu(std::vector<ContextMenuControlEntry> entries, std::function<void(int)> callback, float x, float y) {
      showContextMenu(std::move(entries), std::move(callback), {x, y, 1, 1});
    }
    void transferTo(const files::Location& destination, bool copy) {
      const auto request = std::exchange(m_pendingWorkspaceRequest, std::nullopt);
      if (!request || !files::ActionRegistry::current(actionContext(), *request, files::ActionScope::Targets)) {
        return;
      }
      std::vector<files::Location> sources;
      sources.reserve(request->targets.size());
      for (const auto& target : request->targets) {
        sources.push_back(target.location);
      }
      if (!sources.empty() && !m_transfers.busy()) {
        (void)m_transfers.start(
            {std::move(sources), destination, copy ? files::TransferAction::Copy : files::TransferAction::Move}
        );
      }
    }
    std::optional<std::pair<files::Location, LayoutRect>> foreignTarget(float x, float y) const {
      if (!m_visible || !m_filePane->containsScenePoint(x, y) || overlay()) {
        return std::nullopt;
      }
      if (const auto index = m_list->visible() ? m_list->itemAtScene(x, y) : std::nullopt) {
        const auto& rows = browser().model().rows();
        if (*index >= rows.size()) {
          return std::nullopt;
        }
        const auto* entry = browser().model().entry(rows[*index]);
        if (!entry || entry->symlink || entry->type != G_FILE_TYPE_DIRECTORY || !entry->location.nativePath()) {
          return std::nullopt;
        }
        if (auto rect = m_list->itemRect(*index)) {
          return std::pair(entry->location, *rect);
        }
        return std::nullopt;
      }
      if (location() && location()->nativePath() && browser().searchQuery().empty()) {
        return std::pair(*location(), sceneRect(*m_filePane));
      }
      return std::nullopt;
    }

    void outputChanged(wl_output* output) {
      if (m_connection.outputForSurface(m_surface->wlSurface()) == output) {
        m_surface->onSurfaceOutputEnter(m_surface->wlSurface(), output);
      }
      if (m_menuSurface != nullptr && m_connection.outputForSurface(m_menuSurface->wlSurface()) == output) {
        m_menuSurface->onSurfaceOutputEnter(m_menuSurface->wlSurface(), output);
      }
      if (m_documentSurface && m_connection.outputForSurface(m_documentSurface->wlSurface()) == output) {
        m_documentSurface->onSurfaceOutputEnter(m_documentSurface->wlSurface(), output);
      }
    }

    void pointer(const PointerEvent& event) {
      if (m_menuSurface != nullptr && event.type == PointerEvent::Type::Button && event.pressed) {
        const bool inPopup = (m_documentSurface && event.surface == m_documentSurface->wlSurface())
            || (event.surface == m_menuSurface->wlSurface()
                && event.sx >= 0
                && event.sy >= 0
                && event.sx < m_menuSurface->width()
                && event.sy < m_menuSurface->height());
        if (!inPopup) {
          m_dispatcher.cancelPointerCapture();
          dismissMenu();
          return;
        }
      }
      auto* dispatcher = event.surface == m_surface->wlSurface()                    ? &m_dispatcher
          : m_menuSurface != nullptr && event.surface == m_menuSurface->wlSurface() ? &m_menuDispatcher
          : m_documentSurface && event.surface == m_documentSurface->wlSurface()    ? &m_documentDispatcher
                                                                                    : nullptr;
      if (dispatcher == nullptr) {
        return;
      }
      const auto x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
      if (dispatcher == &m_menuDispatcher && m_batchRenameDialog) {
        m_batchRenameDialog->tooltipPointer(
            x, y, event.type == PointerEvent::Type::Motion || event.type == PointerEvent::Type::Enter
        );
      }
      if (dispatcher == &m_dispatcher
          && event.type == PointerEvent::Type::Button
          && event.pressed
          && event.button == BTN_MIDDLE
          && m_workspaceRequest) {
        if (m_list && m_list->containsScenePoint(x, y)) {
          if (auto index = m_list->itemAtScene(x, y); index && *index < browser().model().rows().size()) {
            const auto* entry = browser().model().entry(browser().model().rows()[*index]);
            if (entry && entry->type == G_FILE_TYPE_DIRECTORY) {
              m_workspaceRequest(WorkspaceCommand::NewTab, entry->location);
            }
            return;
          }
        }
        for (const auto& [location, button] : m_dropBreadcrumbs) {
          if (button->containsScenePoint(x, y)) {
            m_workspaceRequest(WorkspaceCommand::NewTab, location);
            return;
          }
        }
        if (m_places) {
          if (auto folder = m_places->folderAt(x, y)) {
            m_workspaceRequest(WorkspaceCommand::NewTab, folder->location);
            return;
          }
        }
      }

      if ((m_settingsView || m_archiveView) && dispatcher == &m_dispatcher) {
        m_pointerX = x;
        m_pointerY = y;
        if (event.type == PointerEvent::Type::Button && event.pressed) {
          m_popupSerial = event.serial;
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
        m_surface->requestUpdate();
        return;
      }
      m_pointerX = x;
      m_pointerY = y;
      if (dispatcher == &m_dispatcher
          && m_miller.enabled()
          && event.type == PointerEvent::Type::Button
          && event.pressed) {
        if (const auto column = m_millerView->columnAt(x, y)) {
          focusMiller(*column, false);
          m_miller.interact(*column);
        }
      }
      if (dispatcher == &m_dispatcher
          && m_miller.enabled()
          && event.type == PointerEvent::Type::Axis
          && m_filePane->containsScenePoint(x, y)
          && (event.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL || m_connection.keyboardModifiers() == KeyMod::Shift)) {
        const float delta = event.axisValue120 != 0
            ? static_cast<float>(event.axisValue120) / 120.0F * Style::scrollWheelStep
            : event.axisDiscrete != 0 ? static_cast<float>(event.axisDiscrete) * Style::scrollWheelStep
                                      : static_cast<float>(event.axisValue);
        if (std::isfinite(delta)) {
          m_millerView->scrollBy(delta);
        }
        m_surface->requestLayout();
        return;
      }
      if (dispatcher == &m_dispatcher && event.type == PointerEvent::Type::Button && event.button == BTN_LEFT) {
        m_fileDragSerial = event.pressed && !event.touch ? event.serial : 0;
      }
      if (dispatcher == &m_dispatcher
          && m_blankLocationClick
          && (event.type == PointerEvent::Type::Leave
              || event.type == PointerEvent::Type::Axis
              || (event.type == PointerEvent::Type::Motion
                  && (std::abs(x - m_blankLocationClick->x) > 6.0F || std::abs(y - m_blankLocationClick->y) > 6.0F)))) {
        m_blankLocationClick.reset();
      }
      if (dispatcher == &m_dispatcher
          && event.type == PointerEvent::Type::Button
          && event.pressed
          && m_editingLocation
          && m_completionFrame
          && !m_completionFrame->containsScenePoint(x, y)) {
        clearCompletions();
      }
      if (event.type == PointerEvent::Type::Button && event.pressed && dispatcher == &m_dispatcher) {
        clearTypeahead();
      }
      if (event.type == PointerEvent::Type::Button && event.pressed) {
        m_popupSerial = event.serial;
        if (dispatcher == &m_dispatcher) {
          m_fileKeyboardFocus = false;
          m_browserDirty = true;
          m_surface->requestUpdate();
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
          if (header->visible() && header->containsScenePoint(x, y)) {
            column = candidate;
          }
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
            && browser().model().snapshot().location == m_lastBreadcrumb
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
          m_breadcrumbEditTarget = browser().model().snapshot().location;
          m_browserDirty = true;
          m_surface->requestUpdate();
          return;
        }
      }
      if (dispatcher == &m_dispatcher && event.type == PointerEvent::Type::Button && event.pressed) {
        const auto& location = browser().model().snapshot().location;
        const bool blank = event.button == BTN_LEFT
            && m_connection.keyboardModifiers() == 0
            && !m_editingLocation
            && location
            && m_locationArea
            && m_locationArea->containsScenePoint(x, y)
            && !(m_breadcrumbOverflow
                 && m_breadcrumbOverflow->visible()
                 && m_breadcrumbOverflow->containsScenePoint(x, y))
            && std::ranges::none_of(m_dropBreadcrumbs, [x, y](const auto& crumb) {
                             return crumb.second->containsScenePoint(x, y);
                           });
        if (blank) {
          const auto now = std::chrono::steady_clock::now();
          const bool twice = m_blankLocationClick
              && m_blankLocationClick->location == location
              && m_blankLocationClick->context == browser().contextSerial()
              && now - m_blankLocationClick->time < std::chrono::milliseconds(400)
              && std::abs(x - m_blankLocationClick->x) <= 6.0F
              && std::abs(y - m_blankLocationClick->y) <= 6.0F;
          if (twice) {
            m_blankLocationClick.reset();
            m_breadcrumbEditTarget = location;
            m_surface->requestUpdate();
            return;
          }
          m_blankLocationClick = LocationBlankClick{*location, browser().contextSerial(), now, x, y};
        } else {
          m_blankLocationClick.reset();
        }
      }
      if (dispatcher == &m_dispatcher
          && event.type == PointerEvent::Type::Button
          && event.pressed
          && event.button == BTN_LEFT
          && !m_surface->serverDecorated()
          && !m_surface->maximized()
          && !m_surface->fullscreen()) {
        const auto edges = resizeEdges(x, y);
        if (edges != XDG_TOPLEVEL_RESIZE_EDGE_NONE) {
          m_surface->beginResize(event.serial, edges);
          return;
        }
      }
      switch (event.type) {
      case PointerEvent::Type::Enter:
        dispatcher->pointerEnter(x, y, event.serial);
        break;
      case PointerEvent::Type::Leave:
        dispatcher->pointerLeave();
        if (dispatcher == &m_dispatcher && !m_systemDrag) {
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
        if (dispatcher == &m_menuDispatcher && m_documentSurface) {
          dismissDocumentMenu();
        }
        if (dispatcher == &m_dispatcher
            && m_viewMode == files::ViewMode::List
            && m_filePane->containsScenePoint(x, y)
            && (event.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL
                || (event.axis == WL_POINTER_AXIS_VERTICAL_SCROLL
                    && m_connection.keyboardModifiers() == KeyMod::Shift))) {
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
      if (dispatcher == &m_documentDispatcher && event.type != PointerEvent::Type::Leave) {
        m_closeDocumentPending = false;
      }
      if (dispatcher == &m_menuDispatcher
          && (event.type == PointerEvent::Type::Enter || event.type == PointerEvent::Type::Motion)) {
        if (m_documentRow && m_menu && m_menu->highlightedIndex() == *m_documentRow) {
          m_documentKeyboard = false;
          (void)m_menu->activateHighlighted();
        } else if (m_documentSurface) {
          dismissDocumentMenu();
        }
      }
    }

    void keyboard(const KeyboardEvent& event) {
      if (event.pressed) {
        m_popupSerial = m_connection.lastInputSerial();
        m_lastBreadcrumb.reset();
        m_blankLocationClick.reset();
        if ((m_columnDrag || m_columnResize || (m_millerView && m_millerView->resizing()))
            && KeySymbol::isEscape(event.sym)) {
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
      if (m_columnDrag || m_columnResize) {
        return;
      }
      if (m_drag) {
        updateDrag();
        return;
      }
      if (m_menuSurface != nullptr) {
        if (m_quickPreview && event.pressed && !event.preedit && event.modifiers == 0) {
          if (KeySymbol::isSpace(event.sym) || KeySymbol::isEscape(event.sym)) {
            dismissMenu();
            return;
          }
          if (KeySymbol::isLeft(event.sym) || KeySymbol::isRight(event.sym)) {
            const auto& rows = browser().model().rows();
            const auto selected = browser().model().selectedIds();
            if (selected.size() == 1) {
              const auto found = std::ranges::find(rows, selected.front());
              if (found != rows.end()) {
                const auto index =
                    static_cast<std::ptrdiff_t>(found - rows.begin()) + (KeySymbol::isLeft(event.sym) ? -1 : 1);
                if (index >= 0 && static_cast<std::size_t>(index) < rows.size()) {
                  browser().model().select(rows[static_cast<std::size_t>(index)]);
                }
              }
            }
            syncPreview();
            m_menuSurface->requestLayout();
            return;
          }
        }
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
        if (m_documentSurface) {
          if (!event.pressed) {
            return;
          }
          if (KeySymbol::isEscape(event.sym) || KeySymbol::isLeft(event.sym)) {
            dismissDocumentMenu();
          } else if (KeySymbol::isEnter(event.sym)) {
            if (m_documentMenu->highlightedIndex() >= m_documentMenu->entryCount()) {
              (void)m_documentMenu->moveHighlight(1);
            }
            (void)m_documentMenu->activateHighlighted();
          } else if (KeySymbol::isUp(event.sym) || KeySymbol::isDown(event.sym)) {
            (void)m_documentMenu->moveHighlight(KeySymbol::isUp(event.sym) ? -1 : 1);
            const auto index = m_documentMenu->highlightedIndex();
            const float top = m_documentMenu->rowTop(index), bottom = m_documentMenu->rowBottom(index);
            const float offset = m_documentScroll->scrollOffset();
            if (top < offset) {
              m_documentScroll->requestScrollToOffset(top);
            } else if (bottom > offset + m_documentScroll->height()) {
              m_documentScroll->requestScrollToOffset(bottom - m_documentScroll->height());
            }
            m_documentSurface->requestLayout();
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
        } else if (
            KeySymbol::isEnter(event.sym)
            || (KeySymbol::isRight(event.sym) && m_documentRow && m_menu->highlightedIndex() == *m_documentRow)
        ) {
          m_documentKeyboard = true;
          (void)m_menu->activateHighlighted();
        }
        return;
      }
      if (m_archiveView) {
        if (event.pressed && KeySymbol::isEscape(event.sym)) {
          deferCloseArchive();
        } else if (event.pressed && event.sym == XKB_KEY_q && event.modifiers == KeyMod::Ctrl) {
          m_surface->close();
        } else if (event.pressed && KeySymbol::isTab(event.sym)) {
          (void)m_dispatcher.cycleTabFocusInSubtree(m_archiveView, (event.modifiers & KeyMod::Shift) != 0);
        } else if (
            !event.pressed
            || event.preedit
            || m_dispatcher.focusedArea() != m_archiveView->focusArea()
            || !m_archiveView->key(event.sym, event.modifiers)
        ) {
          m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
        }
        m_surface->requestLayout();
        return;
      }
      if (m_settingsView) {
        if (event.pressed
            && (KeySymbol::isEscape(event.sym) || (event.sym == XKB_KEY_comma && event.modifiers == KeyMod::Ctrl))) {
          deferCloseSettings();
        } else if (event.pressed && event.sym == XKB_KEY_q && event.modifiers == KeyMod::Ctrl) {
          m_surface->close();
        } else if (event.pressed && KeySymbol::isTab(event.sym)) {
          (void)m_dispatcher.cycleTabFocusInSubtree(m_settingsView, (event.modifiers & KeyMod::Shift) != 0);
        } else {
          m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
        }
        m_surface->requestLayout();
        return;
      }
      if (event.pressed && !event.preedit) {
        const bool editing = (m_location != nullptr && m_dispatcher.focusedArea() == m_location->inputArea())
            || (m_filterInput != nullptr && m_dispatcher.focusedArea() == m_filterInput->inputArea());
        if (!editing
            && !event.repeat
            && event.modifiers == KeyMod::Ctrl
            && (event.sym == XKB_KEY_c || event.sym == XKB_KEY_x || event.sym == XKB_KEY_v)) {
          dispatchAction(
              event.sym == XKB_KEY_c       ? Action::CopyFiles
                  : event.sym == XKB_KEY_x ? Action::CutFiles
                                           : Action::PasteFiles
          );
          return;
        }
        if (!editing && event.modifiers == 0 && !event.repeat && event.sym == XKB_KEY_F8) {
          dispatchAction(Action::ShowPreview);
          return;
        }
        if (!editing
            && event.modifiers == 0
            && !event.repeat
            && KeySymbol::isSpace(event.sym)
            && m_dispatcher.focusedArea() == m_list->focusArea()) {
          dispatchAction(Action::QuickPreview);
          return;
        }
        if (!m_typeaheadPreedit.empty()) {
          if (KeySymbol::isEscape(event.sym)) {
            clearTypeahead();
          }
          return;
        }
        if ((event.sym == XKB_KEY_Menu && event.modifiers == 0)
            || (event.sym == XKB_KEY_F10 && event.modifiers == KeyMod::Shift)) {
          if (m_breadcrumbOverflow
              && m_breadcrumbOverflow->visible()
              && m_dispatcher.focusedArea() == m_breadcrumbOverflow->inputArea()) {
            openBreadcrumbOverflow();
            return;
          }
          for (const auto& [target, button] : m_dropBreadcrumbs) {
            if (m_dispatcher.focusedArea() == button->inputArea()) {
              openBreadcrumbMenu(target, sceneRect(*button));
              return;
            }
          }
        }
        if (m_editingLocation && completionKey(event.sym, event.modifiers)) {
          return;
        }
        if (KeySymbol::isTab(event.sym)) {
          (void)m_dispatcher.cycleTabFocus((event.modifiers & KeyMod::Shift) != 0);
          m_surface->requestRedraw();
          return;
        }
        if (event.sym == XKB_KEY_F11 && !event.repeat) {
          m_surface->toggleFullscreen();
          return;
        }
        if (event.sym == XKB_KEY_q && event.modifiers == KeyMod::Ctrl) {
          m_surface->close();
          return;
        }
        if (event.sym == XKB_KEY_comma && event.modifiers == KeyMod::Ctrl) {
          dispatchAction(Action::Settings);
          return;
        }
        if (event.sym == XKB_KEY_l && event.modifiers == KeyMod::Ctrl && m_location != nullptr) {
          dispatchAction(Action::EditLocation);
          return;
        }
        if ((event.sym == XKB_KEY_k || event.sym == XKB_KEY_f)
            && event.modifiers == KeyMod::Ctrl
            && m_filterInput != nullptr) {
          dispatchAction(Action::Search);
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
          dispatchAction(Action::Back);
          return;
        }
        if (!editing && event.modifiers == KeyMod::Alt && event.sym == XKB_KEY_Right) {
          dispatchAction(Action::Forward);
          return;
        }
        if (!editing && event.modifiers == KeyMod::Alt && event.sym == XKB_KEY_Up) {
          dispatchAction(Action::Up);
          return;
        }
        if (!editing && event.modifiers == KeyMod::Alt && event.sym == XKB_KEY_Home) {
          dispatchAction(Action::Home);
          return;
        }
        if (!editing && event.sym == XKB_KEY_F5 && event.modifiers == 0) {
          dispatchAction(Action::Refresh);
          return;
        }
        if (!editing
            && m_viewMode == files::ViewMode::List
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
              if (index != 0 && widths[0] == 0 && widths[index] != m_columnWidths[index]) {
                widths[0] = static_cast<int>(std::lround(m_detailsColumns.nameWidth));
              }
              setDetailsColumnWidths(widths);
              m_revealDetailsHeader = widthHeader(index);
              return;
            }
          }
        }
        if (!editing && zoomFromKey(event)) {
          return;
        }
        if (!editing && event.sym == XKB_KEY_F9 && event.modifiers == 0 && !event.repeat) {
          dispatchAction(Action::ShowSidebar);
          return;
        }
        if (!editing
            && (event.sym == XKB_KEY_n || event.sym == XKB_KEY_N)
            && event.modifiers == (KeyMod::Ctrl | KeyMod::Shift)
            && !event.repeat) {
          dispatchAction(Action::CreateFolder);
          return;
        }
        if (!editing
            && ((event.sym == XKB_KEY_Menu && event.modifiers == 0)
                || (event.sym == XKB_KEY_F10 && event.modifiers == KeyMod::Shift))) {
          for (auto* header : {m_nameHeader, m_typeHeader, m_sizeHeader, m_modifiedHeader}) {
            if (m_dispatcher.focusedArea() == header->inputArea()) {
              std::optional<files::DetailsColumn> column;
              for (const auto candidate : m_columnOrder) {
                if (columnHeader(candidate) == header) {
                  column = candidate;
                }
              }
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
          auto id = browser().model().selection().focused;
          float x = 240, y = 100;
          if (id) {
            if (!browser().model().selection().selected.contains(*id)) {
              browser().model().select(*id);
            }
            const auto& rows = browser().model().rows();
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
          if (event.sym == XKB_KEY_Delete && (event.modifiers == 0 || event.modifiers == KeyMod::Shift)) {
            const auto& folder = browser().model().snapshot().location;
            if (folder && folder->isTrash()) {
              dispatchAction(Action::DeletePermanently);
            } else if (event.modifiers == 0) {
              dispatchAction(Action::Trash);
            }
            return;
          }
          if (event.sym == XKB_KEY_F2 && event.modifiers == 0) {
            dispatchAction(Action::Rename);
            return;
          }
          if (event.sym == XKB_KEY_Return && event.modifiers == KeyMod::Alt) {
            dispatchAction(Action::Properties);
            return;
          }
        }
        if (!editing
            && event.modifiers == KeyMod::Ctrl
            && (event.sym == XKB_KEY_1 || event.sym == XKB_KEY_2 || event.sym == XKB_KEY_3)) {
          dispatchAction(
              event.sym == XKB_KEY_1       ? Action::Grid
                  : event.sym == XKB_KEY_2 ? Action::List
                                           : Action::Miller
          );
          return;
        }
        if (!editing && event.sym == XKB_KEY_h && event.modifiers == KeyMod::Ctrl) {
          dispatchAction(Action::ShowHidden);
          return;
        }
        if (m_list != nullptr && m_dispatcher.focusedArea() == m_list->focusArea() && listKey(event)) {
          m_fileKeyboardFocus = !KeySymbol::isEscape(event.sym);
          m_browserDirty = true;
          m_surface->requestUpdate();
          return;
        }
      }
      if (event.pressed
          && !event.preedit
          && m_typingMode == files::TypingMode::Typeahead
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
              browser().model(), static_cast<std::uint32_t>(text.data() + text.size() - previous),
              files::TypeaheadSelection::Clock::now()
          ));
          return;
        }
      }
      if (event.pressed
          && !event.preedit
          && m_typingMode == files::TypingMode::Search
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
          .model = browser().model(),
          .context = browser().contextSerial(),
          .query = browser().searchQuery(),
          .canBack = browser().canBack(),
          .canForward = browser().canForward(),
          .canUp = browser().canUp(),
          .operationsBusy = m_creator.busy()
              || renameBusy()
              || m_archives.busy()
              || m_transfers.busy()
              || m_restore.busy()
              || m_trash.busy()
              || m_trash.canTrash()
              || m_emptyTrash.pending()
              || m_pendingFilePaste,
          .pinsEditable = m_pins.editable(),
          .pinsBusy = m_pins.busy(),
          .canEmptyTrash = canEmptyTrash(),
          .pinned = [this](const auto& location) { return m_pins.contains(location); },
          .viewMode = m_viewMode,
          .showSidebar = m_showSidebar,
          .showPreview = m_savedSettings.showPreview,
          .clipboardAvailable = m_clipboard.available(),
          .clipboardHasFiles = m_clipboard.hasFiles(),
          .terminalAvailable = m_terminal.available(),
          .activationMode = m_activationMode,
          .typingMode = m_typingMode
      };
    }
    files::ActionRequest actionRequest(bool withTargets = true) const {
      if (withTargets) {
        return files::ActionRegistry::capture(actionContext());
      }
      return {browser().contextSerial(), browser().model().snapshot().location, browser().searchQuery(), {}};
    }
    files::ActionRequest requestFor(const std::vector<files::DirectoryEntry>& entries) const {
      auto request = actionRequest(false);
      for (const auto& entry : entries) {
        request.targets.push_back({entry.id, entry.location});
      }
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
          .checkmark = !radio && state.checked != -1,
          .radio = radio,
          .toggleState = state.checked,
          .shortcut = definition.shortcut.empty() ? std::nullopt : std::optional(std::string(definition.shortcut)),
          .destructive = definition.destructive
      };
    }
    void dispatchAction(Action action) {
      dispatchAction(
          action, actionRequest(files::ActionRegistry::definition(action).scope == files::ActionScope::Targets)
      );
    }
    void dispatchAction(Action action, const files::ActionRequest& request) {
      if (action == Action::Open) {
        Logger("file-launch").debug("Open action dispatch ({} targets)", request.targets.size());
      }
      const auto state = actionState(action, request);
      if (!state.enabled) {
        actionError(std::string(state.reason));
        return;
      }
      m_actionError.clear();
      clearTypeahead();
      const bool selectionAction = files::ActionRegistry::definition(action).scope == files::ActionScope::Selection;
      if (selectionAction) {
        if (m_miller.enabled()) {
          m_miller.interact(m_miller.focused());
        }
        m_dispatcher.cancelPointerCapture();
        m_list->cancelPointerGesture();
      }
      std::vector<files::DirectoryEntry> targets;
      for (const auto& target : request.targets) {
        if (const auto* entry = browser().model().entry(target.id)) {
          targets.push_back(*entry);
        }
      }
      switch (action) {
      case Action::CutFiles:
      case Action::CopyFiles: {
        files::FilePayload payload{.locations = {}, .cut = action == Action::CutFiles};
        for (const auto& target : targets) {
          payload.locations.push_back(target.location);
        }
        if (!m_clipboard.setFiles(std::move(payload))) {
          actionError("The file selection could not be copied to the clipboard.");
        }
        break;
      }
      case Action::PasteFiles: {
        m_pendingFilePaste = true;
        const std::weak_ptr<int> lifetime = m_lifetime;
        m_clipboard.requestFiles([this, lifetime, request](std::optional<files::ClipboardFiles> selection) {
          if (lifetime.expired()) {
            return;
          }
          const bool pending = std::exchange(m_pendingFilePaste, false);
          if (!pending
              || !m_visible
              || !files::ActionRegistry::current(actionContext(), request, files::ActionScope::Location)) {
            return;
          }
          if (!selection) {
            actionError("The clipboard changed or its file data could not be read.");
            return;
          }
          if (actionContext().operationsBusy) {
            return;
          }
          if (selection->payload.cut) {
            if (!m_clipboard.claimMove(selection->revision)) {
              return;
            }
            m_clipboardTransfer = *selection;
          }
          if (!startPayloadTransfer(
                  selection->payload, *request.folder,
                  selection->payload.cut ? files::TransferAction::Move : files::TransferAction::Copy
              )) {
            if (const auto claimed = std::exchange(m_clipboardTransfer, std::nullopt)) {
              m_clipboard.finishMove(*claimed, {});
            }
          }
        });
        break;
      }
      case Action::ShowPreview:
        m_savedSettings.showPreview = !m_savedSettings.showPreview;
        m_preferences.submit({{files::BrowserPreference::ShowPreview, m_savedSettings.showPreview}});
        m_surface->requestLayout();
        break;
      case Action::QuickPreview:
        showQuickPreview();
        break;
      case Action::OpenWith:
        showApplications(targets.front());
        break;
      case Action::Compress: {
        std::vector<files::Location> sources;
        sources.reserve(targets.size());
        for (const auto& target : targets) {
          sources.push_back(target.location);
        }
        openArchiveDialog(std::move(sources), {}, true);
        break;
      }
      case Action::ExtractTo:
        openArchiveDialog({targets.front().location}, {}, false);
        break;
      case Action::ExtractHere:
        if (!m_archives.extract(targets.front().location, archiveDestination(targets.front().location))) {
          actionError("Could not start extraction.");
        }
        break;
      case Action::BrowseArchive:
        if (m_archives.inspect(targets.front().location)) {
          m_archiveSource = targets.front().location;
          m_archiveWanted = true;
          m_archiveDirty = true;
        } else {
          actionError("Could not read this archive.");
        }
        break;
      case Action::CopyPath:
      case Action::CopyUri:
      case Action::CopyTargetPath:
      case Action::CopyTargetUri: {
        std::string text;
        const bool uri = action == Action::CopyUri || action == Action::CopyTargetUri;
        const auto append = [&](const files::Location& location) {
          if (!text.empty()) {
            text += '\n';
          }
          text += uri ? location.uri() : files::locationEditText(location);
        };
        if (action == Action::CopyPath || action == Action::CopyUri) {
          append(*request.folder);
        } else {
          for (const auto& target : targets) {
            append(target.location);
          }
        }
        if (text.size() > 1024 * 1024) {
          actionError("The selected paths exceed the clipboard text limit.");
        } else {
          m_clipboard.setClipboardText(std::move(text));
        }
        break;
      }
      case Action::Terminal:
      case Action::TargetTerminal: {
        const auto target = action == Action::Terminal ? *request.folder : targets.front().location;
        const auto context = browser().contextSerial();
        const std::weak_ptr<int> lifetime = m_lifetime;
        m_terminal.open(target, [this, lifetime, context](std::string error) {
          if (!lifetime.expired() && context == browser().contextSerial() && !error.empty()) {
            actionError(std::move(error));
          }
        });
        break;
      }
      case Action::Back:
        (void)browser().back();
        break;
      case Action::Forward:
        (void)browser().forward();
        break;
      case Action::Up:
        (void)browser().up();
        break;
      case Action::Home:
        browser().home();
        break;
      case Action::Root:
        browser().open(files::Location::fromPath("/"));
        break;
      case Action::Refresh:
        browser().refresh();
        break;
      case Action::Stop:
        browser().stop();
        break;
      case Action::EditLocation:
        beginLocationEdit();
        break;
      case Action::Search:
        openFilter();
        break;
      case Action::Grid:
        setViewMode(files::ViewMode::Grid);
        break;
      case Action::List:
        setViewMode(files::ViewMode::List);
        break;
      case Action::Miller:
        setViewMode(files::ViewMode::Miller);
        break;
      case Action::ShowHidden:
        toggleHidden();
        break;
      case Action::ShowSidebar:
        setSidebarVisible(!m_showSidebar);
        break;
      case Action::SortName:
      case Action::SortType:
      case Action::SortSize:
      case Action::SortModified:
      case Action::Ascending:
      case Action::Descending:
      case Action::FoldersFirst: {
        auto options = browser().model().sort();
        const auto field = action == Action::SortName ? files::SortField::Name
            : action == Action::SortType              ? files::SortField::Type
            : action == Action::SortSize              ? files::SortField::Size
                                                      : files::SortField::Modified;
        if (action == Action::Ascending || action == Action::Descending) {
          options.descending = action == Action::Descending;
        } else if (action == Action::FoldersFirst) {
          options.directoriesFirst = !options.directoriesFirst;
        } else if (options.field != field) {
          options.field = field;
          options.descending = false;
        }
        changeSort(options);
        break;
      }
      case Action::Open:
        if (m_miller.enabled()
            && targets.size() == 1
            && targets.front().type == G_FILE_TYPE_DIRECTORY
            && !targets.front().symlink
            && browser().searchQuery().empty()) {
          (void)browser().model().select(targets.front().id);
          if (m_miller.enter()) {
            m_millerFocusPending = true;
          }
        } else {
          for (const auto& target : targets) {
            browser().activate(target.id);
          }
        }
        break;
      case Action::ContainingFolder:
        showContaining(targets.front().location);
        break;
      case Action::OriginalLocation:
        showContaining(*targets.front().trash.original);
        break;
      case Action::Pin: {
        std::vector<files::Bookmark> pins;
        for (const auto& target : targets) {
          if (!m_pins.contains(target.location)) {
            pins.push_back({target.displayName, target.location});
          }
        }
        pinFolders(std::move(pins), m_pins.bookmarks().size());
        break;
      }
      case Action::Unpin: {
        std::vector<files::Location> locations;
        locations.reserve(targets.size());
        for (const auto& target : targets) {
          locations.push_back(target.location);
        }
        m_pins.removeMany(locations);
        break;
      }
      case Action::PinCurrent:
        pinFolders(
            {{files::locationBreadcrumbs(*request.folder).back().label, *request.folder}}, m_pins.bookmarks().size()
        );
        break;
      case Action::UnpinCurrent:
        m_pins.remove(*request.folder);
        break;
      case Action::ReloadPins:
        m_pins.reload();
        break;
      case Action::EmptyTrash:
        openEmptyTrash();
        break;
      case Action::Rename:
        if (targets.size() == 1) {
          openFileRename(targets.front());
        } else {
          openBatchRename(targets);
        }
        break;
      case Action::CreateLink:
        openLinkDialog(targets.front());
        break;
      case Action::Trash:
        trashItems(targets);
        break;
      case Action::Restore:
        cancelDrag();
        m_actionError.clear();
        if (!m_restore.start(targets)) {
          actionError("Could not start restoring the selection.");
        }
        break;
      case Action::DeletePermanently:
        openEmptyTrash(targets);
        break;
      case Action::Properties:
        showProperties(targets);
        break;
      case Action::CreateFolder:
      case Action::CreateFile:
        openCreateItem(
            *request.folder, action == Action::CreateFolder ? files::CreateKind::Folder : files::CreateKind::EmptyFile
        );
        break;
      case Action::NewDocument:
        openDocumentMenu(request);
        break;
      case Action::SelectAll:
        browser().model().selectAll();
        break;
      case Action::InvertSelection:
        (void)browser().model().invertSelection();
        break;
      case Action::ClearSelection:
        m_list->cancelPointerGesture();
        browser().model().clearSelection();
        break;
      case Action::SelectPattern:
        openPatternSelection(request);
        break;
      case Action::SingleClick:
      case Action::DoubleClick:
      case Action::TypeToSearch:
      case Action::Typeahead:
        if (action == Action::SingleClick || action == Action::DoubleClick) {
          m_activationMode =
              action == Action::SingleClick ? files::ActivationMode::SingleClick : files::ActivationMode::DoubleClick;
        } else {
          m_typingMode = action == Action::Typeahead ? files::TypingMode::Typeahead : files::TypingMode::Search;
        }
        for (auto* adapter : {&m_listAdapter, &m_gridAdapter}) {
          adapter->onPointerCancel();
          adapter->setActivationMode(m_activationMode);
        }
        if (state.checked != 1) {
          m_preferences.saveInteraction(m_activationMode, m_typingMode);
        }
        break;
      case Action::Settings:
        openSettings();
        break;
      case Action::About:
        openAbout();
        break;
      case Action::ViewMenu:
        openMenu();
        break;
      case Action::SortMenu:
        openSortMenu();
        break;
      case Action::SelectionMenu:
        openSelectionMenu();
        break;
      case Action::InteractionMenu:
        openInteractionMenu();
        break;
      }
      if (!m_menuSurface
          && (selectionAction
              || action == Action::SingleClick
              || action == Action::DoubleClick
              || action == Action::TypeToSearch
              || action == Action::Typeahead)) {
        m_dispatcher.setFocus(m_list->focusArea());
      }
      m_browserDirty = true;
      m_surface->requestUpdate();
    }
    void openPatternSelection(const files::ActionRequest& request) {
      const float width = std::min(440.0F, static_cast<float>(m_surface->width()) - 32.0F);
      auto editor = std::make_unique<NameEditor>(
          width, "Select by pattern", m_selectionPattern,
          "Use * for any text and ? for one character. Matches visible names, ignoring case.",
          files::selectionPatternProblem,
          [this, request](std::string pattern) {
            const auto state = actionState(Action::SelectPattern, request);
            if (!state.enabled) {
              m_patternEditor->showError(std::string(state.reason));
              return;
            }
            if (const auto error = browser().model().selectPattern(pattern)) {
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
      const float height = editor->measure(m_surface->renderTarget().renderer(), constraints).height;
      showPopup(
          std::move(editor), nullptr,
          {(m_surface->width() - width) * 0.5F, (m_surface->height() - height) * 0.5F, 1, 1}, width, height
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
      m_surface->requestUpdate();
    }

    bool targetsCurrent(const std::vector<files::DirectoryEntry>& targets) {
      const bool current = std::ranges::all_of(targets, [this](const auto& target) {
        const auto* entry = browser().model().entry(target.id);
        return entry != nullptr
            && entry->location == target.location
            && std::ranges::contains(browser().model().rows(), target.id);
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
        if (m_hasOther && m_hasOther()) {
          entries.push_back({.id = 9005, .label = "Close Pane", .shortcut = "Ctrl+Alt+W"});
        }
        add(Action::CreateFolder);
        add(Action::NewDocument);
        entries.back().hasSubmenu = true;
        add(Action::PasteFiles);
        separator();
        add(Action::SelectAll);
        add(Action::InvertSelection);
        add(Action::SelectPattern);
        separator();
        add(Action::Refresh);
        if (request.folder && !request.folder->isTrash()) {
          add(m_pins.contains(*request.folder) ? Action::UnpinCurrent : Action::PinCurrent);
        }
        add(Action::ReloadPins);
        separator();
        add(Action::CopyPath);
        add(Action::CopyUri);
        add(Action::Terminal);
      } else {
        if (request.targets.empty()) {
          return;
        }
        add(Action::Open);
        if (request.targets.size() == 1) {
          add(Action::OpenWith);
          add(Action::QuickPreview);
        }
        if (request.targets.size() == 1) {
          const auto* entry = browser().model().entry(request.targets.front().id);
          if (entry && entry->type == G_FILE_TYPE_DIRECTORY) {
            entries.push_back({.id = 9000, .label = "Open in New Tab"});
            entries.push_back({.id = 9001, .label = "Open in Other Pane"});
            entries.push_back({.id = 9002, .label = "Open in New Window"});
          }
        }
        separator();
        add(Action::CutFiles);
        add(Action::CopyFiles);
        add(Action::PasteFiles);
        entries.push_back(
            {.id = 9003,
             .label = "Copy to Other Pane",
             .enabled = m_hasOther && m_hasOther() && !actionContext().operationsBusy}
        );
        entries.push_back(
            {.id = 9004,
             .label = "Move to Other Pane",
             .enabled = m_hasOther && m_hasOther() && !actionContext().operationsBusy}
        );
        if (request.targets.size() == 1) {
          const auto* entry = browser().model().entry(request.targets.front().id);
          if (entry && files::isSupportedArchive(entry->location, entry->contentType)) {
            add(Action::BrowseArchive);
            add(Action::ExtractHere);
            add(Action::ExtractTo);
          }
        }
        if (request.targets.size() == 1) {
          add(Action::ContainingFolder);
          if (browser().model().entry(request.targets.front().id)->trash.original) {
            add(Action::OriginalLocation);
          }
        }
        const bool folders = std::ranges::all_of(request.targets, [this](const auto& target) {
          const auto* entry = browser().model().entry(target.id);
          return entry->type == G_FILE_TYPE_DIRECTORY && !entry->location.isTrash();
        });
        if (folders) {
          separator();
          if (std::ranges::any_of(request.targets, [this](const auto& target) {
                return !m_pins.contains(target.location);
              })) {
            add(Action::Pin);
          }
          if (std::ranges::any_of(request.targets, [this](const auto& target) {
                return m_pins.contains(target.location);
              })) {
            add(Action::Unpin);
          }
        }
        separator();
        const bool trash = request.targets.front().location.isTrash();
        if (trash) {
          add(Action::Restore);
          add(Action::DeletePermanently);
        }
        if (!trash) {
          add(Action::Rename);
        }
        if (!trash) {
          add(Action::CreateLink);
          add(Action::Compress);
          add(Action::Trash);
        }
        separator();
        add(Action::CopyTargetPath);
        add(Action::CopyTargetUri);
        if (folders && request.targets.size() == 1) {
          add(Action::TargetTerminal);
        }
        separator();
        add(Action::Properties);
      }
      const auto document =
          std::ranges::find(entries, static_cast<int>(Action::NewDocument), &ContextMenuControlEntry::id);
      const auto documentRow = !id && document != entries.end()
          ? std::optional<std::size_t>(static_cast<std::size_t>(document - entries.begin()))
          : std::nullopt;
      showContextMenu(
          std::move(entries),
          [this, request](int action) {
            if (action == 9005) {
              if (m_workspaceRequest) {
                m_workspaceRequest(WorkspaceCommand::ClosePane, std::nullopt);
              }
              return;
            }
            if (action >= 9000) {
              if (!files::ActionRegistry::current(actionContext(), request, files::ActionScope::Targets)
                  || !m_workspaceRequest) {
                return;
              }
              const auto command = action == 9000 ? WorkspaceCommand::NewTab
                  : action == 9001                ? WorkspaceCommand::OpenOther
                  : action == 9002                ? WorkspaceCommand::NewWindow
                  : action == 9003                ? WorkspaceCommand::CopyOther
                                                  : WorkspaceCommand::MoveOther;
              if (action >= 9003) {
                m_pendingWorkspaceRequest = request;
              }
              m_workspaceRequest(
                  command, action < 9003 ? std::optional(request.targets.front().location) : std::nullopt
              );
              return;
            }
            dispatchAction(static_cast<Action>(action), request);
          },
          {x, y, 1, 1}
      );
      if (m_menu && documentRow) {
        m_documentRow = documentRow;
        m_menu->setOnSubmenuOpen([this, request](const ContextMenuControlEntry&, float) { openDocumentMenu(request); });
      }
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
             .enabled = actionState(Action::EmptyTrash, actionRequest(false)).enabled,
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
              browser().open(bookmark.location);
            } else if (action == 2) {
              m_pins.remove(bookmark.location);
            } else if (action == 7 && bookmark.location == files::Location::trash()) {
              dispatchAction(Action::EmptyTrash);
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
                    .parentDisplayName = {}}},
                  true
              );
            }
          },
          {x, y, 1, 1}
      );
    }

    bool canEmptyTrash() const {
      return !m_archives.busy()
          && !m_transfers.busy()
          && !m_restore.busy()
          && !m_trash.busy()
          && !m_trash.canTrash()
          && !m_emptyTrash.pending()
          && !renameBusy()
          && !m_creator.busy();
    }
    void openEmptyTrash(std::optional<std::vector<files::DirectoryEntry>> selection = std::nullopt) {
      if (!canEmptyTrash()) {
        actionError("Another file operation is still finishing.");
        return;
      }
      cancelDrag();
      const float width = std::min(420.0F, static_cast<float>(m_surface->width()) - 32.0F);
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
          ui::label(
              {.text = selection ? "Delete selected items permanently?" : "Empty Trash?",
               .fontWeight = FontWeight::SemiBold}
          ),
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
                   .text = selection ? "Delete permanently" : "Empty Trash",
                   .enabled = false,
                   .variant = ButtonVariant::Destructive,
                   .onClick = [this] { confirmEmptyTrash(); }}
              )
          )
      );
      UiPhaseScope phase(UiPhase::Layout);
      LayoutConstraints constraints;
      constraints.setExactWidth(width);
      const float height = root->measure(m_surface->renderTarget().renderer(), constraints).height;
      showPopup(
          std::move(root), nullptr, {(m_surface->width() - width) * 0.5F, (m_surface->height() - height) * 0.5F, 1, 1},
          width, height
      );
      if (m_menuSurface != nullptr) {
        m_emptyTrashDialog = true;
        m_selectedTrashDelete = selection.has_value();
        m_emptyTrashMessage = message;
        m_emptyTrashSize = size;
        m_emptyTrashConfirm = confirm;
        m_menuDispatcher.setFocus(cancel->inputArea());
        if (!(selection ? m_emptyTrash.prepareSelection(std::move(*selection)) : m_emptyTrash.prepare())) {
          message->setText("Another Trash deletion is still finishing.");
        }
      }
    }
    void confirmEmptyTrash() {
      if (!m_emptyTrashDialog || !m_emptyTrash.empty()) {
        return;
      }
      m_emptyTrashConfirm->setEnabled(false);
      m_emptyTrashDialog = false;
      dismissMenu();
    }
    void syncEmptyTrash() {
      const auto& snapshot = m_emptyTrash.snapshot();
      if (m_emptyTrashDialog && m_emptyTrashMessage != nullptr) {
        if (snapshot.status == files::EmptyTrashStatus::Ready) {
          const auto count = snapshot.results.size();
          m_emptyTrashMessage->setText(
              count == 0 ? "Trash is already empty."
                         : std::format(
                               "Permanently delete {} item{} and their contents? This cannot be undone.", count,
                               count == 1 ? "" : "s"
                           )
          );
          m_emptyTrashConfirm->setEnabled(m_emptyTrash.canEmpty());
        } else if (snapshot.status == files::EmptyTrashStatus::Error) {
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
      } else if (snapshot.status == files::EmptyTrashStatus::Finished) {
        const auto count = [&](files::EmptyTrashOutcome outcome) {
          return std::ranges::count(snapshot.results, outcome, &files::EmptyTrashResult::outcome);
        };
        const auto done = count(files::EmptyTrashOutcome::Deleted);
        std::string message = std::format("{} item{} permanently deleted", done, done == 1 ? "" : "s");
        for (const auto& [outcome, label] :
             {std::pair{files::EmptyTrashOutcome::Skipped, "skipped"},
              std::pair{files::EmptyTrashOutcome::Failed, "failed"},
              std::pair{files::EmptyTrashOutcome::Cancelled, "cancelled"},
              std::pair{files::EmptyTrashOutcome::Unconfirmed, "could not be confirmed"}}) {
          if (const auto amount = count(outcome); amount != 0) {
            message += std::format(" · {} {}", amount, label);
          }
        }
        const auto error = std::ranges::find_if(snapshot.results, [](const auto& item) { return !item.error.empty(); });
        if (error != snapshot.results.end()) {
          message += std::format(" · {}: {}", error->displayName, error->error);
        }
        actionError(std::move(message));
        if (const auto& location = browser().model().snapshot().location; location && location->isTrash()) {
          browser().open(files::Location::trash());
        }
      } else if (snapshot.status == files::EmptyTrashStatus::Error) {
        actionError(
            std::string(m_selectedTrashDelete ? "Could not delete selected items: " : "Could not empty Trash: ")
            + snapshot.error
        );
      }
    }

    void updateOperationProgress() {
      if (m_batchRenamer.applying()
          || m_archives.busy()
          || m_transfers.busy()
          || m_restore.busy()
          || m_trash.busy()
          || m_emptyTrash.busy()) {
        m_operationProgress.startRepeating(std::chrono::milliseconds(100), [this] {
          m_batchRenameDirty = m_batchRenamer.applying();
          m_browserDirty = true;
          m_surface->requestUpdate();
        });
      } else {
        m_operationProgress.stop();
      }
    }
    bool canTrashItems(const std::vector<files::DirectoryEntry>& targets) const {
      return actionState(Action::Trash, requestFor(targets)).enabled;
    }
    void trashItems(const std::vector<files::DirectoryEntry>& targets) {
      if (!canTrashItems(targets) || !targetsCurrent(targets)) {
        actionError("This selection cannot be moved to Trash right now.");
        return;
      }
      cancelDrag();
      std::vector<files::Location> sources;
      sources.reserve(targets.size());
      for (const auto& target : targets) {
        sources.push_back(target.location);
      }
      if (!m_trash.prepare(*browser().model().snapshot().location, std::move(sources))) {
        actionError("Another Trash operation is still finishing.");
      }
    }
    void syncTrash() {
      const auto& snapshot = m_trash.snapshot();
      if (snapshot.status == files::TrashStatus::Ready) {
        (void)m_trash.trash();
      } else if (snapshot.status == files::TrashStatus::Error) {
        actionError("Could not move to Trash: " + snapshot.error);
      } else if (snapshot.status == files::TrashStatus::Cancelled) {
        actionError("Move to Trash cancelled.");
      } else if (snapshot.status == files::TrashStatus::Finished) {
        const auto done =
            std::ranges::count(snapshot.results, files::TrashOutcome::Trashed, &files::TrashResult::outcome);
        const auto failed =
            std::ranges::count(snapshot.results, files::TrashOutcome::Failed, &files::TrashResult::outcome);
        const auto cancelled =
            std::ranges::count(snapshot.results, files::TrashOutcome::Cancelled, &files::TrashResult::outcome);
        std::string message = std::format("{} item{} moved to Trash", done, done == 1 ? "" : "s");
        if (failed != 0) {
          message += std::format(" · {} failed", failed);
        }
        if (cancelled != 0) {
          message += std::format(" · {} cancelled", cancelled);
        }
        const auto error =
            std::ranges::find_if(snapshot.results, [](const auto& result) { return !result.error.empty(); });
        if (error != snapshot.results.end()) {
          const files::GString raw(g_file_get_basename(error->source.file().get()));
          const files::GString name(g_filename_display_name(raw ? raw.get() : "Item"));
          message += std::format(" · {}: {}", name.get(), error->error);
        }
        actionError(std::move(message));
        browser().refresh();
      }
    }

    bool canCreateItem() const { return actionState(Action::CreateFolder, actionRequest(false)).enabled; }
    void closeDocumentMenu() {
      if (m_menu) {
        m_menu->setSubmenuDirection(ContextSubmenuDirection::Right);
      }
      ++m_documentGeneration;
      m_closeDocumentPending = false;
      m_templates.cancel();
      m_documentDispatcher.setSceneRoot(nullptr);
      if (m_documentSurface) {
        m_documentSurface->setSceneRoot(nullptr);
      }
      m_documentRoot.reset();
      m_documentMenu = nullptr;
      m_documentScroll = nullptr;
      m_documentSurface.reset();
    }
    void dismissDocumentMenu() {
      m_closeDocumentPending = true;
      const auto generation = m_documentGeneration;
      const std::weak_ptr<int> lifetime = m_lifetime;
      DeferredCall::callLater([this, lifetime, generation] {
        if (!lifetime.expired() && generation == m_documentGeneration && m_closeDocumentPending) {
          closeDocumentMenu();
        }
      });
    }
    void fillDocumentMenu(const files::ActionRequest& request, files::TemplateListing listing, bool loading = false) {
      if (!m_documentSurface || !actionState(Action::NewDocument, request).enabled) {
        return;
      }
      auto items = std::make_shared<std::vector<files::DocumentTemplate>>(std::move(listing.items));
      std::vector<ContextMenuControlEntry> entries{
          {.id = -1, .label = "Empty File…"}, {.label = "", .separator = true}
      };
      for (std::size_t index = 0; index < items->size(); ++index) {
        entries.push_back({.id = static_cast<int>(index), .label = (*items)[index].label});
      }
      if (loading || items->empty() || !listing.error.empty()) {
        entries.push_back(
            {.label = loading             ? "Loading templates…"
                 : !listing.error.empty() ? listing.error
                                          : "No document templates",
             .enabled = false}
        );
      }
      if (listing.truncated) {
        entries.push_back({.label = "Template list limit reached", .enabled = false});
      }
      const float contentHeight = ContextMenuControl::preferredHeight(entries, entries.size());
      const float height = std::min(contentHeight, static_cast<float>(m_surface->height()) - 32.0F);
      const float width = 300.0F;
      const bool overflowing = contentHeight > height;
      m_documentMenu->setMaxVisible(entries.size());
      m_documentMenu->setEntries(std::move(entries));
      m_documentMenu->setMenuWidth(width - (overflowing ? Style::scrollbarWidth + Style::scrollbarGap : 0.0F));
      m_documentMenu->setHighlightedIndex(m_documentKeyboard ? 0 : m_documentMenu->entryCount());
      m_documentMenu->setOnActivate([this, request, items](const ContextMenuControlEntry& entry) {
        const auto id = entry.id;
        const std::weak_ptr<int> lifetime = m_lifetime;
        dismissMenu();
        DeferredCall::callLater([this, lifetime, request, items, id] {
          if (lifetime.expired() || !actionState(Action::NewDocument, request).enabled) {
            return;
          }
          if (id == -1) {
            openCreateItem(*request.folder, files::CreateKind::EmptyFile);
          } else if (id >= 0 && static_cast<std::size_t>(id) < items->size()) {
            openCreateItem(*request.folder, files::CreateKind::Document, (*items)[static_cast<std::size_t>(id)]);
          }
        });
      });
      m_documentScroll->setReserveScrollbarGutter(overflowing);
      m_documentScroll->setSize(width, height);
      (void)m_documentSurface->resize(
          static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(std::ceil(height)), false
      );
      m_documentSurface->requestLayout();
    }
    void openDocumentMenu(const files::ActionRequest& request) {
      m_closeDocumentPending = false;
      if (m_documentSurface
          || !m_menuSurface
          || !m_menu
          || !m_documentRow
          || !actionState(Action::NewDocument, request).enabled) {
        return;
      }
      const float top = m_menu->rowTop(*m_documentRow);
      const float bottom = m_menu->rowBottom(*m_documentRow);
      const auto menuRect = sceneRect(*m_menu);
      m_documentSurface = std::make_unique<PopupSurface>(m_connection);
      m_documentSurface->setRenderContext(&m_renderer);
      m_documentSurface->setAnimationManager(&m_animations);
      m_documentSurface->setDismissedCallback([this] { dismissDocumentMenu(); });
      auto menu = std::make_unique<ContextMenuControl>();
      m_documentMenu = menu.get();
      auto scroll = ui::scrollView(
          {.out = &m_documentScroll, .viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F, .flexGrow = 1.0F}
      );
      scroll->content()->addChild(std::move(menu));
      m_documentRoot = ui::column(
          {.fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh), .radius = Style::radiusLg}, std::move(scroll)
      );
      m_documentRoot->setAnimationManager(&m_animations);
      m_documentSurface->setSceneRoot(m_documentRoot.get());
      m_documentDispatcher.setSceneRoot(m_documentRoot.get());
      m_documentDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
        m_connection.extension<files::DesktopIntegration>().setCursorShape(serial, shape);
      });
      m_documentSurface->setConfigureCallback([this](std::uint32_t, std::uint32_t) {
        if (m_menu) {
          m_menu->setSubmenuDirection(
              m_documentSurface->configuredX() < 0 ? ContextSubmenuDirection::Left : ContextSubmenuDirection::Right
          );
          m_menuSurface->requestLayout();
        }
        m_documentSurface->requestLayout();
      });
      m_documentSurface->setPrepareFrameCallback([this](bool, bool) {
        if (m_documentSurface && m_renderer.makeCurrent(m_documentSurface->renderTarget())) {
          UiPhaseScope phase(UiPhase::Layout);
          m_documentRoot->arrange(
              m_documentSurface->renderTarget().renderer(),
              {0, 0, static_cast<float>(m_documentSurface->width()), static_cast<float>(m_documentSurface->height())}
          );
        }
      });
      const PopupSurfaceConfig config{
          .anchorX = 0,
          .anchorY = static_cast<int>(std::floor(menuRect.y + top)),
          .anchorWidth = static_cast<int>(m_menuSurface->width()),
          .anchorHeight = std::max(1, static_cast<int>(bottom - top)),
          .width = 300,
          .height = 84,
          .anchor = XDG_POSITIONER_ANCHOR_TOP_RIGHT,
          .gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT,
          .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
              | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
              | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y,
          .serial = m_popupSerial != 0 ? m_popupSerial : m_connection.lastInputSerial()
      };
      if (!m_documentSurface->initializeAsChild(
              m_menuSurface->xdgSurface(), m_connection.outputForSurface(m_surface->wlSurface()), config
          )) {
        closeDocumentMenu();
        return;
      }
      fillDocumentMenu(request, {}, true);
      const auto generation = ++m_documentGeneration;
      m_templates.load([this, request, generation](files::TemplateListing listing) {
        if (generation == m_documentGeneration && m_documentSurface) {
          fillDocumentMenu(request, std::move(listing));
        }
      });
    }
    void openCreateItem(
        files::Location parent, files::CreateKind kind, std::optional<files::DocumentTemplate> source = std::nullopt
    ) {
      if (!canCreateItem() || browser().model().snapshot().location != parent) {
        actionError("Open an available writable folder to create a file or folder.");
        return;
      }
      const float width = std::min(420.0F, static_cast<float>(m_surface->width()) - 32.0F);
      const bool document = kind == files::CreateKind::Document;
      const files::GString display(source ? g_filename_display_name(source->name.c_str()) : nullptr);
      auto editor = std::make_unique<NameEditor>(
          width,
          kind == files::CreateKind::Folder ? "New folder"
              : document                    ? "New document"
                                            : "New file",
          source                                  ? display.get()
              : kind == files::CreateKind::Folder ? "New Folder"
                                                  : "New File",
          source ? "From " + source->label : "", files::filenameProblem,
          [this](std::string name) { submitCreateItem(std::move(name)); }, [this] { dismissMenu(); }, "Create",
          "Creating…"
      );
      auto* control = editor.get();
      UiPhaseScope phase(UiPhase::Layout);
      LayoutConstraints constraints;
      constraints.setExactWidth(width);
      const float height = editor->measure(m_surface->renderTarget().renderer(), constraints).height;
      showPopup(
          std::move(editor), nullptr,
          {(m_surface->width() - width) * 0.5F, (m_surface->height() - height) * 0.5F, 1, 1}, width, height
      );
      if (m_menuSurface != nullptr) {
        m_createEditor = control;
        m_createParent = std::move(parent);
        m_creationPreparing = true;
        control->setSaving(true);
        if (!(source ? m_creator.prepareTemplate(*m_createParent, std::move(*source))
                     : m_creator.prepare(*m_createParent, kind))) {
          control->showError("Another creation is still finishing.");
        }
      }
    }
    void submitCreateItem(std::string name) {
      if (m_createEditor == nullptr || !m_createParent) {
        return;
      }
      if (browser().model().snapshot().location != m_createParent || !browser().searchQuery().empty()) {
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
    void openLinkDialog(const files::DirectoryEntry& target) {
      if (!actionState(Action::CreateLink, requestFor({target})).enabled || !targetsCurrent({target})) {
        actionError("This item cannot be linked right now.");
        return;
      }
      const files::GioPtr<GFile> parent(g_file_get_parent(target.location.file().get()));
      if (!parent) {
        actionError("The source has no containing folder.");
        return;
      }
      const files::GString uri(g_file_get_uri(parent.get()));
      const auto base = files::Location::fromUri(uri.get());
      const float width = std::min(480.0F, static_cast<float>(m_surface->width()) - 32.0F);
      auto root = std::make_unique<LinkDialog>(
          width, files::locationEditText(target.location), "Link to " + target.displayName,
          files::locationEditText(base), target.type == G_FILE_TYPE_REGULAR && !target.symlink,
          [this, base](std::string name, const std::string& path, files::CreateKind kind) {
            if (!m_linkDialog) {
              return;
            }
            try {
              const auto parent = files::resolveLocationText(path, base);
              if (m_creator.createLink(parent, std::move(name), kind)) {
                m_linkDialog->setSaving(true);
              } else {
                m_linkDialog->showError(
                    m_creator.snapshot().error.empty() ? "The source is unavailable. Close this dialog and try again."
                                                       : m_creator.snapshot().error
                );
              }
            } catch (const std::exception& error) {
              m_linkDialog->showError(error.what());
            }
            m_menuSurface->requestLayout();
          },
          [this] { dismissMenu(); }
      );
      auto* dialog = root.get();
      UiPhaseScope phase(UiPhase::Layout);
      LayoutConstraints constraints;
      constraints.setExactWidth(width);
      const float height = std::min(
          root->measure(m_surface->renderTarget().renderer(), constraints).height,
          static_cast<float>(m_surface->height()) - 64.0F
      );
      showPopup(
          std::move(root), nullptr, {(m_surface->width() - width) * 0.5F, (m_surface->height() - height) * 0.5F, 1, 1},
          width, height
      );
      if (m_menuSurface) {
        m_linkDialog = dialog;
        m_linkPreparing = true;
        m_dialogHeight = height;
        m_dialogWidth = width;
        m_menuDispatcher.setFocusChangeCallback([this](InputArea*, InputArea* focused) {
          if (m_linkDialog) {
            m_linkDialog->reveal(focused);
            m_menuSurface->requestLayout();
          }
        });
        dialog->setSaving(true);
        if (!m_creator.prepareLink(target)) {
          dialog->setSaving(false);
          dialog->showError("Another creation is still finishing.");
        }
      }
    }
    void syncCreation() {
      if (m_creator.busy()) {
        return;
      }
      const auto& snapshot = m_creator.snapshot();
      if (snapshot.status == files::CreateStatus::Succeeded || snapshot.status == files::CreateStatus::Partial) {
        m_actionError = snapshot.error;
        if (m_linkDialog && snapshot.error.empty() && snapshot.parent) {
          m_actionError = "Link created in " + files::locationEditText(*snapshot.parent);
        }
        if (browser().model().snapshot().location == snapshot.parent) {
          m_revealTarget = snapshot.destination;
          browser().refresh();
        }
        if (m_createEditor != nullptr || m_linkDialog != nullptr) {
          m_createEditor = nullptr;
          dismissMenu();
        }
      } else if (m_linkDialog) {
        m_linkDialog->setSaving(false);
        m_linkDialog->showError(snapshot.error);
        if (std::exchange(m_linkPreparing, false)) {
          m_menuDispatcher.setFocus(m_linkDialog->nameInput()->inputArea());
          m_linkDialog->nameInput()->selectAll();
        }
        m_menuSurface->requestLayout();
      } else if (m_createEditor == nullptr) {
        if (snapshot.status == files::CreateStatus::Error) {
          actionError("Could not create: " + snapshot.error);
        }
      } else if (snapshot.status == files::CreateStatus::Ready || snapshot.status == files::CreateStatus::Error) {
        m_createEditor->setSaving(false);
        m_createEditor->showError(snapshot.error);
        if (std::exchange(m_creationPreparing, false)) {
          m_menuDispatcher.setFocus(m_createEditor->input()->inputArea());
          if (snapshot.kind == files::CreateKind::Document) {
            const auto& value = m_createEditor->input()->value();
            m_createEditor->input()->selectRange(0, files::filenameStemLength(value, false));
          } else {
            m_createEditor->input()->selectAll();
          }
        }
        m_menuSurface->requestLayout();
      }
    }

    void openBatchRename(const std::vector<files::DirectoryEntry>& targets) {
      if (!actionState(Action::Rename, requestFor(targets)).enabled || !targetsCurrent(targets)) {
        actionError("This selection cannot be renamed right now.");
        return;
      }
      auto root = std::make_unique<BatchRenameDialog>(
          targets.size(),
          [this](files::BatchRenameOptions options) { (void)m_batchRenamer.preview(std::move(options)); },
          [this] { (void)m_batchRenamer.apply(); },
          [this] {
            if (m_batchRenamer.applying()) {
              m_batchRenamer.cancel();
            } else {
              dismissMenu();
            }
          },
          [this] { dismissMenu(); }, [this] { m_clipboard.setClipboardText(m_batchRenamer.snapshot().report); },
          [this] {
            if (m_menuSurface) {
              m_menuSurface->requestUpdate();
            }
          }
      );
      auto* dialog = root.get();
      const float width = std::min(760.0F, static_cast<float>(m_surface->width()) - 32.0F);
      const float height = std::min(560.0F, static_cast<float>(m_surface->height()) - 64.0F);
      root->setSize(width, height);
      showPopup(
          std::move(root), nullptr, {(m_surface->width() - width) * 0.5F, (m_surface->height() - height) * 0.5F, 1, 1},
          width, height
      );
      if (m_menuSurface) {
        m_batchRenameDialog = dialog;
        m_batchRenameReported = false;
        m_dialogHeight = height;
        m_dialogWidth = width;
        m_menuDispatcher.setFocusChangeCallback([this](InputArea*, InputArea* focused) {
          if (m_batchRenameDialog) {
            m_batchRenameDialog->reveal(focused);
            m_menuSurface->requestLayout();
          }
        });
        m_menuDispatcher.setFocus(dialog->firstInput()->inputArea());
        dialog->firstInput()->selectAll();
        if (!m_batchRenamer.prepare(targets)) {
          actionError("Another rename is still finishing.");
          dismissMenu();
        }
      }
    }
    void syncBatchRename() {
      const auto& snapshot = m_batchRenamer.snapshot();
      if (m_batchRenameDialog) {
        m_batchRenameDialog->sync(snapshot, m_batchRenamer.completed(), m_clipboard.available());
        m_menuSurface->requestLayout();
      }
      if (snapshot.phase == files::BatchRenamePhase::Finished && !m_batchRenameReported) {
        m_batchRenameReported = true;
        const auto count = std::ranges::count(
            *snapshot.results, files::BatchRenameOutcome::Renamed, &files::BatchRenameResult::outcome
        );
        std::string message = std::format("{} items renamed.", count);
        const auto problem =
            std::ranges::find_if(*snapshot.results, [](const auto& result) { return !result.error.empty(); });
        if (problem != snapshot.results->end()) {
          message += " " + problem->error;
        }
        actionError(std::move(message));
        if (snapshot.parent == browser().model().snapshot().location) {
          m_batchReveal.clear();
          for (const auto& result : *snapshot.results) {
            if (result.actual) {
              m_batchReveal.push_back(*result.actual);
            }
          }
          browser().refresh();
          m_batchRevealContext = browser().contextSerial();
        }
      } else if (snapshot.phase == files::BatchRenamePhase::Error && !m_batchRenameDialog) {
        actionError("Could not rename: " + snapshot.error);
      }
    }

    bool canFileRename(const files::DirectoryEntry& entry) const {
      return actionState(Action::Rename, requestFor({entry})).enabled;
    }
    void openFileRename(files::DirectoryEntry target) {
      if (!canFileRename(target) || !targetsCurrent({target})) {
        actionError("This item cannot be renamed right now.");
        return;
      }
      const float width = std::min(420.0F, static_cast<float>(m_surface->width()) - 32.0F);
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
      const float height = editor->measure(m_surface->renderTarget().renderer(), constraints).height;
      const LayoutRect anchor{(m_surface->width() - width) * 0.5F, (m_surface->height() - height) * 0.5F, 1, 1};
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
      if (m_fileRenameEditor == nullptr || !m_fileRenameTarget) {
        return;
      }
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
      if (m_renamer.busy()) {
        return;
      }
      if (snapshot.status == files::RenameStatus::Succeeded) {
        m_actionError.clear();
        const auto& current = browser().model().snapshot().location;
        if (current && snapshot.source && g_file_has_prefix(snapshot.source->file().get(), current->file().get())) {
          m_revealTarget = snapshot.destination;
          browser().refresh();
        }
        if (m_fileRenameEditor != nullptr) {
          m_fileRenameEditor = nullptr;
          dismissMenu();
        }
      } else if (m_fileRenameEditor == nullptr) {
        if (snapshot.status == files::RenameStatus::Error) {
          actionError("Could not rename: " + snapshot.error);
        }
      } else if (snapshot.status == files::RenameStatus::Ready || snapshot.status == files::RenameStatus::Error) {
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
      if (bookmark.location.isTrash()) {
        return;
      }
      const auto found = std::ranges::find(m_pins.bookmarks(), bookmark.location, &files::Bookmark::location);
      if (!m_pins.editable() || found == m_pins.bookmarks().end()) {
        actionError("This place is unavailable for editing. Reload Places and try again.");
        return;
      }
      const auto current = *found; // NOLINT(performance-unnecessary-copy-initialization): own the editing snapshot.
      const float width = std::min(440.0F, static_cast<float>(m_surface->width()) - 32.0F);
      auto editor = std::make_unique<NameEditor>(
          width, "Rename place", current.label, "Changes the sidebar label only.",
          [](const std::string& label) -> std::optional<std::string> {
            if (const auto error = files::bookmarkLabelProblem(label)) {
              return error;
            }
            if (label.find_first_not_of(' ') == std::string::npos) {
              return "Enter a label.";
            }
            return std::nullopt;
          },
          [this](std::string label) { submitPlaceRename(std::move(label)); }, [this] { dismissMenu(); }
      );
      auto* control = editor.get();
      UiPhaseScope phase(UiPhase::Layout);
      LayoutConstraints constraints;
      constraints.setExactWidth(width);
      const float height = std::min(
          editor->measure(m_surface->renderTarget().renderer(), constraints).height,
          static_cast<float>(m_surface->height()) - 64.0F
      );
      showPopup(
          std::move(editor), nullptr,
          {(m_surface->width() - width) * 0.5F, (m_surface->height() - height) * 0.5F, 1, 1}, width, height
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
      browser().open(files::Location::fromUri(uri.get()));
      m_revealTarget = location;
      m_dispatcher.setFocus(m_list->focusArea());
    }

    void applySettings(const files::BrowserSettings& settings, bool force) {
      const auto before = m_savedSettings;
      if (!force && before == settings) {
        return;
      }
      captureViewAnchor();
      m_dispatcher.cancelPointerCapture();
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      if (m_menu && !m_settingsView) {
        closeMenu();
      }
      if (m_settingsView && (force || before.viewMode != settings.viewMode)) {
        m_viewMode = settings.viewMode;
        browser().setDefaultViewMode(settings.viewMode);
      }
      if (m_settingsView && (force || before.gridIconSize != settings.gridIconSize)) {
        m_gridZoom = static_cast<std::size_t>(
            std::ranges::find(files::kGridIconSizes, settings.gridIconSize) - files::kGridIconSizes.begin()
        );
        m_zoomScroll = 0;
        browser().setDefaultIconSize(settings.gridIconSize);
      }
      if (m_settingsView && (force || before.sort != settings.sort)) {
        browser().model().setSort(settings.sort);
      }
      if (m_settingsView
          && (force || before.showHidden != settings.showHidden)
          && browser().model().filter().showHidden != settings.showHidden) {
        browser().toggleHidden();
      }
      if (force || before.millerColumnWidth != settings.millerColumnWidth) {
        m_miller.defaultWidth = static_cast<float>(settings.millerColumnWidth);
        if (m_settingsView && m_miller.enabled()) {
          m_miller.columns()[m_miller.focused()]->width = m_miller.defaultWidth;
          m_miller.columns()[m_miller.focused()]->fixedWidth = false;
        }
      }
      if (force || before.activationMode != settings.activationMode || before.typingMode != settings.typingMode) {
        clearTypeahead();
        m_activationMode = settings.activationMode;
        m_typingMode = settings.typingMode;
        for (auto* adapter : {&m_listAdapter, &m_gridAdapter}) {
          adapter->setActivationMode(m_activationMode);
        }
      }
      if (force || before.showSidebar != settings.showSidebar) {
        setSidebarVisible(settings.showSidebar, false);
      }
      if (force || before.sidebarWidth != settings.sidebarWidth) {
        m_sidebarWidth = settings.sidebarWidth;
      }
      if (m_settingsView && (force || before.columns != settings.columns)) {
        m_columnVisibility = settings.columns;
      }
      if (m_settingsView && (force || before.columnOrder != settings.columnOrder)) {
        m_columnOrder = settings.columnOrder;
      }
      if (m_settingsView && (force || before.columnWidths != settings.columnWidths)) {
        m_columnWidths = settings.columnWidths;
      }
      if (!m_cachePaused && (force || before.thumbnails != settings.thumbnails)) {
        m_thumbnails.setOptions(settings.thumbnails);
      }
      if (before.millerShowParents != settings.millerShowParents && m_miller.enabled()) {
        m_restartMiller = true;
      }
      m_savedSettings = settings;
      m_browserDirty = true;
      m_surface->requestLayout();
    }

    void showCenteredDialog(std::unique_ptr<Flex> root, float preferredWidth = 588.0F) {
      const float width = std::min(preferredWidth, static_cast<float>(m_surface->width()) - 32.0F);
      UiPhaseScope phase(UiPhase::Layout);
      LayoutConstraints constraints;
      constraints.setExactWidth(width);
      const float preferredHeight = root->measure(m_surface->renderTarget().renderer(), constraints).height;
      const float height = std::min(preferredHeight, static_cast<float>(m_surface->height()) - 64.0F);
      showPopup(
          std::move(root), nullptr, {(m_surface->width() - width) * 0.5F, (m_surface->height() - height) * 0.5F, 1, 1},
          width, height
      );
      if (!m_menuSurface) {
        return;
      }
      m_dialogHeight = preferredHeight;
      m_dialogWidth = preferredWidth;
      m_menuDispatcher.setFocusChangeCallback([this](InputArea*, InputArea* focused) {
        if (m_aboutScroll) {
          m_aboutScroll->requestScrollIntoView(focused);
        }
        if (m_appChoiceScroll) {
          m_appChoiceScroll->requestScrollIntoView(focused);
        }
        if (m_menuSurface) {
          m_menuSurface->requestLayout();
        }
      });
      (void)m_menuDispatcher.cycleTabFocus(false);
      m_menuSurface->requestLayout();
    }

    static files::Location archiveDestination(const files::Location& source) {
      const files::GioPtr<GFile> parent(g_file_get_parent(source.file().get()));
      const files::GioPtr<GFile> child(g_file_get_child(parent.get(), files::archiveFolderName(source).c_str()));
      const files::GString uri(g_file_get_uri(child.get()));
      return files::Location::fromUri(uri.get());
    }
    void openArchiveDialog(std::vector<files::Location> sources, std::vector<std::string> selected, bool compress) {
      if (sources.empty() || m_archives.busy()) {
        return;
      }
      const auto formats = files::archiveFormats();
      if (compress && formats.empty()) {
        actionError("No archive writer is available.");
        return;
      }
      const auto source = sources.front();
      const files::GioPtr<GFile> parentFile(g_file_get_parent(source.file().get()));
      if (!parentFile) {
        actionError("The source has no destination parent.");
        return;
      }
      const files::GString uri(g_file_get_uri(parentFile.get()));
      const auto base = files::Location::fromUri(uri.get());
      const files::GString raw(g_file_get_basename(source.file().get()));
      const files::GString display(g_filename_display_name(raw ? raw.get() : "Archive"));
      const std::string name = (sources.size() == 1 ? std::string(display.get()) : "Archive")
          + (formats.empty() ? ".zip" : std::string(formats.front().suffix));
      auto root = std::make_unique<ArchiveDialog>(
          compress, name, files::locationEditText(compress ? base : archiveDestination(source)),
          [this, sources = std::move(sources), selected = std::move(selected), base,
           compress](const std::string& name, const std::string& path, files::ArchiveFormat format) -> std::string {
            if (path.empty()) {
              return "Enter a destination folder.";
            }
            auto destination = files::resolveLocationText(path, base);
            if (!destination.nativePath()) {
              return "Choose a native destination folder.";
            }
            if (compress) {
              if (auto problem = files::filenameProblem(name)) {
                return *problem;
              }
              const auto formats = files::archiveFormats();
              const auto found = std::ranges::find(formats, format, &files::ArchiveFormatInfo::format);
              if (found == formats.end() || !name.ends_with(found->suffix)) {
                return "Use the filename extension for the selected format.";
              }
              const files::GioPtr<GFile> child(g_file_get_child(destination.file().get(), name.c_str()));
              const files::GString uri(g_file_get_uri(child.get()));
              destination = files::Location::fromUri(uri.get());
            }
            const bool started = compress ? m_archives.compress(sources, destination, format)
                                          : m_archives.extract(sources.front(), destination, selected);
            if (!started) {
              return "Could not start the archive operation.";
            }
            dismissMenu();
            return {};
          },
          [this] { dismissMenu(); }
      );
      auto* dialog = root.get();
      const float width = std::min(520.0F, static_cast<float>(m_surface->width()) - 32.0F);
      UiPhaseScope phase(UiPhase::Layout);
      LayoutConstraints constraints;
      constraints.setExactWidth(width);
      const float height = std::min(
          root->measure(m_surface->renderTarget().renderer(), constraints).height,
          static_cast<float>(m_surface->height()) - 48.0F
      );
      showPopup(
          std::move(root), nullptr, {(m_surface->width() - width) * 0.5F, (m_surface->height() - height) * 0.5F, 1, 1},
          width, height
      );
      if (m_menuSurface) {
        m_dialogHeight = height;
        m_menuDispatcher.setFocus(dialog->focusArea());
      }
    }
    void deferCloseArchive() {
      const std::weak_ptr<int> lifetime = m_lifetime;
      DeferredCall::callLater([this, lifetime] {
        if (!lifetime.expired()) {
          closeArchive();
        }
      });
    }
    void closeArchive() {
      m_archiveWanted = false;
      if (!m_archiveView) {
        return;
      }
      closeMenu();
      m_dispatcher.cancelPointerCapture();
      m_dispatcher.setFocus(m_list->focusArea());
      auto* page = std::exchange(m_archiveView, nullptr);
      auto removed = m_root->removeChild(page);
      m_surface->requestLayout();
    }
    void syncArchive() {
      UiPhaseScope phase(UiPhase::Layout);
      if (m_archiveWanted && !m_archiveView) {
        closeSettings();
        closeMenu();
        cancelDrag();
        clearCompletions();
        if (m_editingLocation) {
          endLocationEdit(false);
        }
        auto root = std::make_unique<ArchiveView>(
            m_icons, [this] { return m_connection.keyboardModifiers(); }, [this] { deferCloseArchive(); },
            [this](std::vector<std::string> selected) {
              if (m_archives.catalog()) {
                openArchiveDialog({m_archives.catalog()->source}, std::move(selected), false);
              }
            },
            [this](const std::string& path) { (void)m_archives.openMember(path); },
            [this] {
              if (m_archiveSource) {
                (void)m_archives.inspect(*m_archiveSource);
              }
            },
            [this] { m_archives.cancel(); }
        );
        m_archiveView = root.get();
        root->setParticipatesInLayout(false);
        root->setZIndex(1000);
        m_root->addChild(std::move(root));
        m_dispatcher.cancelPointerCapture();
        m_dispatcher.setFocus(m_archiveView->focusArea());
      }
      if (m_archiveView) {
        m_archiveView->sync(
            m_archives.catalog(), m_archives.phase(), m_archives.items(), m_archives.bytes(), m_archives.error()
        );
        m_surface->requestLayout();
      }
    }

    void deferCloseSettings() {
      const std::weak_ptr<int> lifetime = m_lifetime;
      DeferredCall::callLater([this, lifetime] {
        if (!lifetime.expired()) {
          closeSettings();
        }
      });
    }
    void closeSettings() {
      if (!m_settingsView) {
        return;
      }
      closeMenu();
      m_settingsLifetime.reset();
      m_dispatcher.cancelPointerCapture();
      m_dispatcher.setFocus(m_list->focusArea());
      auto* settings = std::exchange(m_settingsView, nullptr);
      auto removed = m_root->removeChild(settings);
      m_surface->requestLayout();
    }
    void openSettings() {
      if (m_settingsView) {
        deferCloseSettings();
        return;
      }
      closeArchive();
      closeMenu();
      cancelDrag();
      clearCompletions();
      if (m_editingLocation) {
        endLocationEdit(false);
      }
      m_settingsLifetime = std::make_shared<int>(0);
      auto root = std::make_unique<BrowserSettingsView>(
          [this](files::OverrideEdit edit) {
            m_preferences.submit({edit});
            m_surface->requestLayout();
          },
          [this] { m_preferences.resetAll(); }, [this] { deferCloseSettings(); },
          [this] {
            if (m_clearThumbnails) {
              m_clearThumbnails();
            }
          }
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
      m_surface->requestLayout();
    }
    void openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) override {
      if (!m_settingsView) {
        return;
      }
      std::vector<ContextMenuControlEntry> entries;
      entries.reserve(request.options.size());
      for (std::size_t index = 0; index < request.options.size(); ++index) {
        entries.push_back(
            {.id = static_cast<int>(index),
             .label = request.options[index],
             .checkmark = true,
             .toggleState = index == request.selectedIndex ? 1 : 0}
        );
      }
      const std::weak_ptr<int> lifetime = m_settingsLifetime;
      showContextMenu(
          std::move(entries),
          [lifetime, select = callbacks.onSelect](int index) {
            if (!lifetime.expired() && select) {
              select(static_cast<std::size_t>(index));
            }
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
        if (!lifetime.expired() && dismiss) {
          dismiss();
        }
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

    void syncPreview() {
      const bool quick = m_quickPreview && m_menuSurface;
      if ((!m_previewBounds && !quick) || !m_active || overlay() || m_cachePaused) {
        m_preview.clear();
        return;
      }
      const auto selected = browser().model().selectedIds();
      const auto* entry = selected.size() == 1 && browser().model().selectionAvailable()
          ? browser().model().entry(selected.front())
          : nullptr;
      const auto prepared = entry ? m_thumbnails.prepared(*entry) : nullptr;
      const auto& current = m_preview.snapshot();
      const auto image = entry
              && current.entry
              && entry->id == current.entry->id
              && entry->location == current.entry->location
              && current.image
              && current.image->pixels
          ? current.image
          : prepared;
      const auto* pixels = image && image->pixels ? &*image->pixels : nullptr;
      const float width = quick ? static_cast<float>(m_menuSurface->width()) : m_previewBounds->width;
      const float height = quick ? static_cast<float>(m_menuSurface->height()) : m_previewBounds->height;
      const auto& renderer = quick ? m_menuSurface->renderTarget().renderer() : m_surface->renderTarget().renderer();
      const auto layout = files::previewImageLayout(
          pixels ? pixels->width : 0, pixels ? pixels->height : 0, width, height, renderer.renderScale(), quick
      );
      m_preview.show(entry ? std::optional(*entry) : std::nullopt, layout.pixels, m_savedSettings.thumbnails, prepared);
    }
    void previewOpen(bool with) {
      const auto entry = m_preview.snapshot().entry;
      if (!entry) {
        return;
      }
      const auto request = requestFor({*entry});
      const std::weak_ptr<int> lifetime = m_lifetime;
      DeferredCall::callLater([this, lifetime, request, entry, with] {
        if (lifetime.expired() || !m_active) {
          return;
        }
        if (!actionState(with ? Action::OpenWith : Action::Open, request).enabled) {
          actionError("The previewed item changed. Select it again.");
          return;
        }
        if (with) {
          showApplications(*entry);
        } else {
          dismissMenu();
          dispatchAction(Action::Open, request);
        }
      });
    }
    std::unique_ptr<FilePreviewView> makePreviewView(bool quick) {
      return std::make_unique<FilePreviewView>(
          m_icons,
          [this, quick] {
            if (quick) {
              dismissMenu();
            } else {
              dispatchAction(Action::ShowPreview);
            }
          },
          [this] { previewOpen(false); }, [this] { previewOpen(true); }
      );
    }
    void showQuickPreview() {
      const float width = std::min(588.0F, static_cast<float>(m_surface->width()) - 32);
      const float height = std::min(620.0F, static_cast<float>(m_surface->height()) - 48);
      auto view = makePreviewView(true);
      auto* control = view.get();
      view->setFlexGrow(1);
      auto root = ui::column({.align = FlexAlign::Stretch, .height = height}, std::move(view));
      showPopup(
          std::move(root), nullptr, {(m_surface->width() - width) * 0.5F, (m_surface->height() - height) * 0.5F, 1, 1},
          width, height
      );
      if (m_menuSurface) {
        m_quickPreview = control;
        syncPreview();
      }
    }
    void showApplications(const files::DirectoryEntry& entry) {
      Flex* choices = nullptr;
      ScrollView* choiceScroll = nullptr;
      auto scroll = ui::scrollView(
          {.out = &choiceScroll, .viewportPaddingH = 0, .viewportPaddingV = 0, .fillWidth = true, .flexGrow = 1}
      );
      scroll->content()->setAlign(FlexAlign::Stretch);
      scroll->content()->addChild(
          ui::column(
              {.out = &choices, .align = FlexAlign::Stretch, .gap = Style::spaceXs},
              ui::label({.text = "Loading applications…", .maxLines = 2})
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
              ui::label({.text = "Open With", .fontWeight = FontWeight::SemiBold, .flexGrow = 1}),
              ui::button({.glyph = "x", .variant = ButtonVariant::Ghost, .onClick = [this] { dismissMenu(); }})
          ),
          std::move(scroll)
      );
      showCenteredDialog(std::move(root), 440.0F);
      if (!m_menuSurface) {
        return;
      }
      m_appChoiceContainer = choices;
      m_appChoiceScroll = choiceScroll;
      const auto generation = m_menuGeneration;
      const auto request = requestFor({entry});
      const std::weak_ptr<int> lifetime = m_lifetime;
      m_launcher.applications(
          entry.contentType.value_or("application/octet-stream"),
          [this, lifetime, generation, request, entry](std::vector<files::ApplicationChoice> apps) {
            if (lifetime.expired() || !m_appChoiceContainer || generation != m_menuGeneration || !m_active) {
              return;
            }
            while (!m_appChoiceContainer->children().empty()) {
              (void)m_appChoiceContainer->removeChild(m_appChoiceContainer->children().back().get());
            }
            if (!actionState(Action::OpenWith, request).enabled) {
              apps.clear();
            }
            if (apps.empty()) {
              m_appChoiceContainer->addChild(
                  ui::label({.text = "No applications available for this file type.", .maxLines = 3})
              );
            }
            for (const auto& app : apps) {
              auto button = ui::button(
                  {.text = app.name,
                   .controlHeight = Style::controlHeight,
                   .contentAlign = ButtonContentAlign::Start,
                   .variant = ButtonVariant::Ghost,
                   .tooltip = app.name,
                   .gap = Style::spaceSm,
                   .onClick =
                       [this, request, entry, app] {
                         if (actionState(Action::OpenWith, request).enabled) {
                           m_launcher.openWith(entry.location, app);
                         } else {
                           actionError("The item changed. Select it again.");
                         }
                         dismissMenu();
                       },
                   .configure = [](Button& button) { button.label()->setUseMarkup(false); }}
              );
              button->setFillWidth(true);
              auto source = files::readIconSource(g_app_info_get_icon(app.app.get()));
              source.names.emplace_back("application-x-executable");
              auto icon = std::make_unique<files::FileIcon>(m_icons, 24.0F);
              icon->bind(source, colorSpecFromRole(ColorRole::OnSurface));
              button->insertChildAt(0, std::move(icon));
              m_appChoiceContainer->addChild(std::move(button));
            }
            m_menuSurface->requestLayout();
          }
      );
    }
    void syncPermissions() {
      const auto& snapshot = m_permissions.snapshot();
      if (m_permissionEditor) {
        m_permissionEditor->sync(snapshot);
        m_permissionSave->setEnabled(m_permissionEditor->canApply() && !actionContext().operationsBusy);
        m_permissionSave->setText(snapshot.phase == files::PermissionPhase::Saving ? "Saving…" : "Apply");
        if (snapshot.error && snapshot.phase == files::PermissionPhase::Ready) {
          m_permissionTabs->setSelectedIndex(1);
          m_permissionScroll->requestScrollIntoView(m_permissionEditor->messageNode());
        }
        m_menuSurface->requestLayout();
      }
      if (snapshot.phase == files::PermissionPhase::Ready && snapshot.applied) {
        browser().refresh();
      }
      if (!m_permissions.busy() && !snapshot.message.empty() && (!m_permissionEditor || snapshot.applied)) {
        actionError(snapshot.message);
      }
    }
    void showProperties(const std::vector<files::DirectoryEntry>& targets, bool captureCurrentLocation = false) {
      if (m_permissions.busy()) {
        actionError("A Properties request is still finishing.");
        return;
      }
      const bool editable = targets.size() == 1;
      const float width = std::min(editable ? 600.0F : 440.0F, static_cast<float>(m_surface->width()) - 32.0F);
      ScrollView* scroll = nullptr;
      auto body = ui::scrollView(
          {.out = &scroll, .viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F, .fillWidth = true, .flexGrow = 1.0F}
      );
      body->setReserveScrollbarGutter(true);
      body->content()->setAlign(FlexAlign::Stretch);
      body->content()->setGap(Style::spaceMd);
      auto general = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceMd});
      auto* generalGroup = general.get();
      const auto literal = [](Label& label) { label.setUseMarkup(false); };
      const auto field = [&](const std::string& name, const std::string& value) {
        general->addChild(
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
        general->addChild(
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
        general->addChild(ui::separator({}));
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
      body->content()->addChild(std::move(general));
      PermissionEditor* editor = nullptr;
      Segmented* tabs = nullptr;
      Button* save = nullptr;
      if (editable) {
        auto permissions = std::make_unique<PermissionEditor>([this] {
          if (m_permissionEditor && m_permissionSave) {
            m_permissionSave->setEnabled(m_permissionEditor->canApply() && !actionContext().operationsBusy);
          }
          m_menuSurface->requestLayout();
        });
        editor = permissions.get();
        permissions->setVisible(false);
        permissions->setParticipatesInLayout(false);
        body->content()->addChild(std::move(permissions));
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
          ui::segmented(
              {.out = &tabs,
               .options =
                   std::vector<ui::SegmentedOption>{
                       {.label = "General", .glyph = {}, .tooltip = {}},
                       {.label = "Permissions", .glyph = {}, .tooltip = {}}
                   },
               .equalSegmentWidths = true,
               .visible = editable,
               .participatesInLayout = editable,
               .onChange =
                   [this, generalGroup, editor, scroll](std::size_t index) {
                     generalGroup->setVisible(index == 0);
                     generalGroup->setParticipatesInLayout(index == 0);
                     editor->setVisible(index == 1);
                     editor->setParticipatesInLayout(index == 1);
                     scroll->requestScrollToOffset(0);
                     m_menuSurface->requestLayout();
                   }}
          ),
          std::move(body)
      );
      if (editable) {
        root->addChild(
            ui::row(
                {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
                ui::button({.text = "Close", .variant = ButtonVariant::Ghost, .onClick = [this] { dismissMenu(); }}),
                ui::button(
                    {.out = &save,
                     .text = "Apply",
                     .enabled = false,
                     .variant = ButtonVariant::Primary,
                     .onClick = [this] {
                       if (m_permissionEditor && !actionContext().operationsBusy) {
                         (void)m_permissions.apply(m_permissionEditor->edit());
                       }
                     }}
                )
            )
        );
      }
      UiPhaseScope phase(UiPhase::Layout);
      LayoutConstraints constraints;
      constraints.setExactWidth(width);
      const float height = editable ? std::min(600.0F, static_cast<float>(m_surface->height()) - 64.0F)
                                    : std::min(
                                          root->measure(m_surface->renderTarget().renderer(), constraints).height,
                                          static_cast<float>(m_surface->height()) - 64.0F
                                      );
      if (editable) {
        root->setSize(width, height);
      }
      showPopup(
          std::move(root), nullptr, {(m_surface->width() - width) * 0.5F, (m_surface->height() - height) * 0.5F, 1, 1},
          width, height
      );
      if (m_menuSurface && editable) {
        m_permissionEditor = editor;
        m_permissionTabs = tabs;
        m_permissionSave = save;
        m_permissionScroll = scroll;
        m_dialogWidth = width;
        m_dialogHeight = height;
        m_menuDispatcher.setFocusChangeCallback([this](InputArea*, InputArea* focused) {
          if (m_permissionScroll) {
            m_permissionScroll->requestScrollIntoView(focused);
            m_menuSurface->requestLayout();
          }
        });
        if (captureCurrentLocation) {
          (void)m_permissions.prepare(targets.front().location);
        } else {
          (void)m_permissions.prepare(targets.front());
        }
      }
    }

    void syncTransferConflict() {
      if (m_popupAvailable && !m_popupAvailable()) {
        return;
      }
      const bool archive = m_archives.conflict().has_value();
      const bool restore = m_restore.conflict().has_value();
      const auto conflict = restore ? m_restore.conflict() : archive ? m_archives.conflict() : m_transfers.conflict();
      if (m_activeTransferConflict && !conflict) {
        dismissMenu();
        return;
      }
      if (!conflict || m_menuSurface != nullptr) {
        return;
      }
      const float width = std::min(520.0F, static_cast<float>(m_surface->width()) - 32.0F);
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
      const auto choose = [this, archive, restore, id = conflict->id](files::TransferConflictChoice choice) {
        if (restore       ? m_restore.resolve(id, choice)
                : archive ? m_archives.resolve(id, choice)
                          : m_transfers.resolve(id, choice)) {
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
          ui::label(
              {.text = restore ? files::locationEditText(conflict->destination) : "",
               .fontSize = Style::fontSizeCaption,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxWidth = width - 24.0F,
               .maxLines = 2,
               .visible = restore,
               .participatesInLayout = restore,
               .configure = [](Label& path) { path.setUseMarkup(false); }}
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
              {.text = restore ? "Folder replacement is unavailable. Keep both or skip this item."
                               : "Replace removes the existing folder and its contents.",
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
                   .onClick = [choose] { choose(files::TransferConflictChoice::Cancel); }}
              ),
              ui::button(
                  {.text = "Skip",
                   .variant = ButtonVariant::Ghost,
                   .onClick = [choose] { choose(files::TransferConflictChoice::Skip); }}
              ),
              ui::button(
                  {.out = &keepBoth,
                   .text = "Keep both",
                   .variant = ButtonVariant::Primary,
                   .onClick = [choose] { choose(files::TransferConflictChoice::KeepBoth); }}
              ),
              ui::button(
                  {.text = "Replace",
                   .enabled = !restore || (!conflict->sourceDirectory && !conflict->destinationDirectory),
                   .variant = ButtonVariant::Destructive,
                   .tooltip = restore && (conflict->sourceDirectory || conflict->destinationDirectory)
                       ? "Folder replacement is unavailable; keep both or skip."
                       : "Replace the existing item",
                   .onClick = [choose] { choose(files::TransferConflictChoice::Replace); }}
              )
          )
      );
      UiPhaseScope phase(UiPhase::Layout);
      LayoutConstraints constraints;
      constraints.setExactWidth(width);
      const float height = std::min(
          root->measure(m_surface->renderTarget().renderer(), constraints).height,
          static_cast<float>(m_surface->height()) - 64.0F
      );
      showPopup(
          std::move(root), nullptr, {(m_surface->width() - width) * 0.5F, (m_surface->height() - height) * 0.5F, 1, 1},
          width, height
      );
      if (m_menuSurface != nullptr) {
        m_activeTransferConflict = conflict->id;
        m_transferConflictArchive = archive;
        m_transferConflictRestore = restore;
        m_menuDispatcher.setFocus(keepBoth->inputArea());
      } else {
        (void)(restore       ? m_restore.resolve(conflict->id, files::TransferConflictChoice::Cancel)
                   : archive ? m_archives.resolve(conflict->id, files::TransferConflictChoice::Cancel)
                             : m_transfers.resolve(conflict->id, files::TransferConflictChoice::Cancel));
      }
    }

    bool
    startPayloadTransfer(files::FilePayload payload, const files::Location& destination, files::TransferAction action) {
      if (payload.locations.empty()
          || !destination.nativePath()
          || std::ranges::any_of(payload.locations, [](const auto& location) { return !location.nativePath(); })) {
        actionError("File transfers currently support native files and folders outside Trash.");
        return false;
      }
      m_actionError.clear();
      if (!m_transfers.start({std::move(payload.locations), destination, action})) {
        actionError("Another file transfer is still finishing.");
        return false;
      }
      return true;
    }

    bool dragCurrent() const {
      if (!m_drag) {
        return false;
      }
      if (m_drag->sourcePin) {
        return m_pins.editable() && m_pins.contains(*m_drag->sourcePin);
      }
      return !m_archives.busy()
          && !m_transfers.busy()
          && !m_restore.busy()
          && !m_trash.busy()
          && !m_trash.canTrash()
          && !m_emptyTrash.pending()
          && !renameBusy()
          && !m_creator.busy()
          && std::ranges::all_of(m_drag->entries, [this](const auto& target) {
               const auto* entry = browser().model().entry(target.id);
               return entry != nullptr
                   && entry->location == target.location
                   && std::ranges::contains(browser().model().rows(), target.id);
             });
    }

    bool validDropFolder(const files::Location& destination) const {
      if (m_externalHover) {
        if (!destination.nativePath()) {
          return false;
        }
        const auto* payload = m_clipboard.dragFiles();
        if (!payload) {
          return true;
        }
        bool differentParent = false;
        for (const auto& source : payload->locations) {
          if (source == destination || g_file_has_prefix(destination.file().get(), source.file().get())) {
            return false;
          }
          const files::GioPtr<GFile> parent(g_file_get_parent(source.file().get()));
          differentParent |= !parent || !g_file_equal(parent.get(), destination.file().get());
        }
        return differentParent || (m_connection.keyboardModifiers() & KeyMod::Ctrl) != 0;
      }
      if (!m_drag || m_drag->sourcePin || !destination.nativePath()) {
        return false;
      }
      bool anyDifferentParent = false;
      for (const auto& source : m_drag->entries) {
        if (!source.location.nativePath() || source.location == destination) {
          return false;
        }
        const auto* entry = browser().model().entry(source.id);
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
      return anyDifferentParent || (m_connection.keyboardModifiers() & KeyMod::Ctrl) != 0;
    }

    std::optional<DropTarget> dropTarget() const {
      if (!m_externalHover && (!m_drag || m_drag->sourcePin)) {
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
      if (m_up->containsScenePoint(m_pointerX, m_pointerY) && browser().model().snapshot().location) {
        const files::GioPtr<GFile> parent(g_file_get_parent(browser().model().snapshot().location->file().get()));
        if (parent) {
          const files::GString uri(g_file_get_uri(parent.get()));
          const auto location = files::Location::fromUri(uri.get());
          if (validDropFolder(location)) {
            return DropTarget{location, sceneRect(*m_up)};
          }
        }
      }
      if (!m_externalHover && m_foreignDrop) {
        if (auto target = m_foreignDrop(m_pointerX, m_pointerY); target && validDropFolder(target->first)) {
          return DropTarget{target->first, target->second};
        }
      }
      if (!m_filePane->containsScenePoint(m_pointerX, m_pointerY)) {
        return std::nullopt;
      }
      if (m_miller.enabled()) {
        if (const auto index = m_millerView->columnAt(m_pointerX, m_pointerY)) {
          const auto* grid = m_millerView->view(*index);
          const auto& model = m_miller.columns()[*index]->browser->model();
          if (const auto row = grid->itemAtScene(m_pointerX, m_pointerY)) {
            if (*row < model.rows().size()) {
              const auto* entry = model.entry(model.rows()[*row]);
              if (entry
                  && entry->type == G_FILE_TYPE_DIRECTORY
                  && !entry->symlink
                  && validDropFolder(entry->location)) {
                return DropTarget{entry->location, *grid->itemRect(*row), entry->id};
              }
            }
            return std::nullopt;
          }
          const auto& folder = model.snapshot().location;
          if (folder && validDropFolder(*folder)) {
            return DropTarget{*folder, sceneRect(*grid)};
          }
        }
        return std::nullopt;
      }
      if (m_list->visible()) {
        if (const auto index = m_list->itemAtScene(m_pointerX, m_pointerY)) {
          const auto& rows = browser().model().rows();
          if (*index < rows.size()) {
            const auto* entry = browser().model().entry(rows[*index]);
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
      const auto& location = browser().model().snapshot().location;
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
          || m_archives.busy()
          || m_transfers.busy()
          || m_restore.busy()
          || m_trash.busy()
          || m_trash.canTrash()
          || m_emptyTrash.pending()
          || renameBusy()
          || m_creator.busy()) {
        return;
      }
      Drag drag;
      drag.foldersOnly = true;
      for (const auto id : ids) {
        const auto* entry = browser().model().entry(id);
        if (entry == nullptr
            || entry->location.isTrash()
            || (entry->type != G_FILE_TYPE_DIRECTORY && entry->type != G_FILE_TYPE_REGULAR && !entry->symlink)) {
          return;
        }
        drag.foldersOnly = drag.foldersOnly && entry->type == G_FILE_TYPE_DIRECTORY && !entry->symlink;
        drag.entries.push_back({id, entry->location});
        drag.bookmarks.push_back({entry->displayName, entry->location});
      }
      const auto* entry = browser().model().entry(ids.front());
      if (entry) {
        m_dragArtwork = m_thumbnails.prepared(*entry);
        if (!m_dragArtwork || !m_dragArtwork->pixels) {
          m_dragArtwork = m_icons.request(
              entry->icon,
              static_cast<int>(std::ceil(
                  (m_viewMode == files::ViewMode::Grid ? gridIconSize() : 24) * m_surface->effectiveBufferScale()
              ))
          );
        }
      }
      m_drag = std::move(drag);
      files::FilePayload payload;
      for (const auto& target : m_drag->entries) {
        payload.locations.push_back(target.location);
      }
      const std::weak_ptr<int> lifetime = m_lifetime;
      const auto cursorSerial = m_connection.extension<files::DesktopIntegration>().pointerEnterSerial();
      m_systemDrag = m_clipboard.startDrag(
          std::move(payload), m_surface->wlSurface(), m_fileDragSerial,
          m_dragArtwork && m_dragArtwork->pixels ? &*m_dragArtwork->pixels : nullptr,
          [this, lifetime] {
            if (!lifetime.expired()) {
              m_systemDrag = false;
              cancelDrag();
            }
          },
          (m_connection.keyboardModifiers() & KeyMod::Ctrl) != 0        ? std::optional(files::TransferAction::Copy)
              : (m_connection.keyboardModifiers() & KeyMod::Shift) != 0 ? std::optional(files::TransferAction::Move)
                                                                        : std::nullopt,
          [this, lifetime, cursorSerial](std::optional<files::TransferAction> action) {
            if (!lifetime.expired()) {
              const auto shape = action == files::TransferAction::Copy ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY
                  : action == files::TransferAction::Move              ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE
                                                                       : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP;
              m_connection.extension<files::DesktopIntegration>().setCursorShape(cursorSerial, shape);
            }
          }
      );
      if (m_systemDrag) {
        m_dispatcher.cancelPointerCapture();
        m_connection.extension<files::DesktopIntegration>().setCursorShape(
            cursorSerial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP
        );
        m_surface->requestLayout();
      } else {
        updateDrag();
      }
    }
    void startPlaceDrag(const files::Bookmark& bookmark) {
      if (!m_pins.editable()) {
        return;
      }
      m_drag = Drag{.entries = {}, .bookmarks = {bookmark}, .sourcePin = bookmark.location, .foldersOnly = true};
      updateDrag();
    }
    void cancelDrag() {
      if (m_systemDrag) {
        m_systemDrag = false;
        m_clipboard.cancelDrag();
      }
      if (!m_drag) {
        return;
      }
      m_drag.reset();
      m_dragArtwork.reset();
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
      m_connection.extension<files::DesktopIntegration>().setCursorShape(
          m_connection.lastInputSerial(), WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT
      );
      m_surface->requestLayout();
    }
    VirtualGridView* dragView() const {
      if (m_miller.enabled()) {
        if (const auto index = m_millerView->columnAt(m_pointerX, m_pointerY)) {
          return m_millerView->view(*index);
        }
      }
      return m_list;
    }
    float dragScrollSpeed(bool& places) const {
      places = m_showSidebar && m_places->containsScenePoint(m_pointerX, m_pointerY);
      if (places) {
        return m_places->dragScrollSpeed(m_pointerX, m_pointerY);
      }
      if ((!m_drag && !m_externalDraggingHere) || (m_drag && m_drag->sourcePin) || !m_list->visible()) {
        return 0;
      }
      auto& scroll = dragView()->scrollView();
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
    void updateExternalScroll() {
      bool places = false;
      if (!m_externalDraggingHere || dragScrollSpeed(places) == 0 || m_externalScroll.active()) {
        return;
      }
      m_externalScroll.startRepeating(std::chrono::milliseconds(16), [this] {
        if (!m_externalDraggingHere) {
          m_externalScroll.stop();
          return;
        }
        bool places = false;
        const float speed = dragScrollSpeed(places);
        if (speed == 0) {
          m_externalScroll.stop();
          return;
        }
        m_externalScrollInPlaces = places;
        m_externalScrollDelta = std::clamp(m_externalScrollDelta + speed, -18.0F, 18.0F);
        m_surface->requestLayout();
      });
    }
    void updateDrag() {
      if (m_systemDrag) {
        return;
      }
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
          m_surface->requestLayout();
        });
      }
      m_surface->requestLayout();
    }
    void layoutDrag(Renderer& renderer) {
      m_dragOverlay->setFrameSize(static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height()));
      if (m_drag && !m_systemDrag && !dragCurrent()) {
        cancelDrag();
      }
      const bool active = m_drag.has_value() && !m_systemDrag;
      m_dragIcon->setVisible(active);
      m_dragCount->setVisible(active && m_drag->bookmarks.size() > 1);
      m_placeDropLine->setVisible(false);
      m_folderDropHighlight->setVisible(false);
      if (!active) {
        if (m_externalPinSlot) {
          m_placeDropLine->setVisible(true);
          m_placeDropLine->arrange(renderer, m_places->dropLine(*m_externalPinSlot));
        }
        if (m_externalDropTarget) {
          m_folderDropHighlight->setVisible(true);
          m_folderDropHighlight->arrange(renderer, m_externalDropTarget->rect);
        }
        return;
      }
      files::FileIconSource icon{.names = {"folder"}, .filePath = {}, .desktopFile = {}};
      if (m_drag->sourcePin == files::Location::trash()) {
        icon.names = {"user-trash"};
      }
      if (!m_drag->entries.empty()) {
        if (const auto* entry = browser().model().entry(m_drag->entries.front().id)) {
          icon = entry->icon;
        }
      }
      m_dragIcon->bind(icon, colorSpecFromRole(ColorRole::OnSurface));
      const float x = std::clamp(m_pointerX + 12, 0.0F, static_cast<float>(m_surface->width()) - 48.0F);
      const float y = std::clamp(m_pointerY + 12, 0.0F, static_cast<float>(m_surface->height()) - 48.0F);
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
      const bool copy = (m_connection.keyboardModifiers() & KeyMod::Ctrl) != 0;
      const auto shape = slot
          ? (m_drag->sourcePin ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY)
          : m_lastDropTarget ? (copy ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE)
                             : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP;
      m_connection.extension<files::DesktopIntegration>().setCursorShape(m_connection.lastInputSerial(), shape);
    }
    void finishDrag(float x, float y) {
      m_pointerX = x;
      m_pointerY = y;
      const bool current = dragCurrent();
      const auto target = dropTarget();
      const auto previous = m_lastDropTarget;
      const auto slot = pinDropSlot();
      const auto drag = *m_drag;
      const bool copy = (m_connection.keyboardModifiers() & KeyMod::Ctrl) != 0;
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
            {std::move(sources), target->location, copy ? files::TransferAction::Copy : files::TransferAction::Move}
        );
      } else if (slot) {
        pinFolders(drag.bookmarks, *slot);
      }
    }
    void clearTypeahead() {
      if (m_typeahead.text().empty() && m_typeaheadPreedit.empty()) {
        return;
      }
      m_typeahead.clear();
      m_typeaheadPreedit.clear();
      m_typeaheadTimer.stop();
      m_browserDirty = true;
      m_surface->requestUpdate();
      m_textInput.notifyClientStateChanged(this, TextInputChangeCause::Other);
    }
    void
    typeaheadChanged(std::optional<files::EntryId> match, TextInputChangeCause cause = TextInputChangeCause::Other) {
      if (match) {
        m_keyboardReveal = match;
      }
      m_typeaheadContext = browser().contextSerial();
      m_fileKeyboardFocus = true;
      m_browserDirty = true;
      m_surface->requestUpdate();
      m_typeaheadTimer.start(files::TypeaheadSelection::timeout, [this] {
        if (m_typeaheadPreedit.empty()) {
          clearTypeahead();
        }
      });
      m_textInput.notifyClientStateChanged(this, cause);
    }
    void applyTypeahead(
        const std::string& text, TextInputChangeCause cause = TextInputChangeCause::Other, bool composing = false
    ) {
      m_dispatcher.cancelPointerCapture();
      m_list->cancelPointerGesture();
      typeaheadChanged(
          m_typeahead.append(browser().model(), text, files::TypeaheadSelection::Clock::now(), composing), cause
      );
    }
    TextInputState textInputState() const override {
      if (m_typingMode == files::TypingMode::Search) {
        return m_filterInput->textInputState();
      }
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
      if (m_typingMode == files::TypingMode::Typeahead) {
        const auto now = files::TypeaheadSelection::Clock::now();
        if (edit.hasDelete
            && edit.deleteBeforeLength != 0
            && edit.deleteAfterLength == 0
            && !m_typeahead.text().empty()) {
          typeaheadChanged(
              m_typeahead.erase(browser().model(), edit.deleteBeforeLength, now, !m_typeaheadPreedit.empty()),
              TextInputChangeCause::InputMethod
          );
        }
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
          m_typeaheadContext = browser().contextSerial();
          if (!m_typeaheadPreedit.empty()) {
            m_typeaheadTimer.stop();
          } else {
            typeaheadChanged(std::nullopt, TextInputChangeCause::InputMethod);
          }
          m_browserDirty = true;
          m_surface->requestUpdate();
        }
        return;
      }
      if ((edit.hasCommitText && !edit.commitText.empty())
          || (edit.hasPreedit && !edit.preeditText.empty())
          || (edit.hasDelete && !browser().searchQuery().empty())) {
        openFilter(false, true);
        m_filterInput->textInputApplyEdit(edit);
      }
    }
    void textInputResetPreedit() override {
      if (!m_typeaheadPreedit.empty()) {
        m_typeaheadPreedit.clear();
        m_browserDirty = true;
        m_surface->requestUpdate();
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
      m_surface->requestLayout();
    }
    void requestCompletions(const std::string& text) {
      clearCompletions();
      if (!m_editingLocation || !browser().model().snapshot().location || m_location->composing()) {
        return;
      }
      const auto edit = m_location->textInputState();
      if (edit.cursor != static_cast<std::int32_t>(text.size()) || edit.anchor != edit.cursor) {
        return;
      }
      const auto context = browser().contextSerial();
      m_pathCompletion.complete(
          text, *browser().model().snapshot().location, browser().model().filter().showHidden,
          [this, text, context](files::PathSuggestions suggestions) {
            if (!m_editingLocation
                || context != browser().contextSerial()
                || m_location->value() != text
                || m_location->composing()) {
              return;
            }
            const auto edit = m_location->textInputState();
            if (edit.cursor != static_cast<std::int32_t>(text.size()) || edit.anchor != edit.cursor) {
              return;
            }
            m_completionText = text;
            m_completionContext = context;
            m_completionItems = std::move(suggestions);
            m_completionIndex.reset();
            m_completionDirty = true;
            m_surface->requestLayout();
          }
      );
    }
    void acceptCompletion(std::size_t index, bool navigate) {
      if (index >= m_completionItems.items.size()) {
        return;
      }
      const auto suggestion = m_completionItems.items[index];
      clearCompletions();
      if (navigate) {
        browser().open(suggestion.location);
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
          || m_completionContext != browser().contextSerial()
          || m_location->value() != m_completionText
          || edit.cursor != static_cast<std::int32_t>(m_completionText.size())
          || edit.anchor != edit.cursor) {
        clearCompletions();
        return false;
      }
      if (modifiers != 0 && (!KeySymbol::isTab(key) || modifiers != KeyMod::Shift)) {
        return false;
      }
      if (m_completionItems.items.empty()) {
        return false;
      }
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
        m_surface->requestLayout();
        return true;
      }
      if (KeySymbol::isLeft(key) || KeySymbol::isRight(key) || KeySymbol::isHome(key) || KeySymbol::isEnd(key)) {
        clearCompletions();
      }
      return false;
    }
    void layoutCompletions(Renderer& renderer) {
      if (m_completionDirty) {
        m_completionDirty = false;
        std::vector<ContextMenuControlEntry> entries;
        constexpr std::size_t pageSize = 6;
        const auto page = m_completionIndex.value_or(0) / pageSize * pageSize;
        for (auto i = page; i < std::min(page + pageSize, m_completionItems.items.size()); ++i) {
          entries.push_back({.id = static_cast<int>(i), .label = m_completionItems.items[i].label});
        }
        if (m_completionItems.items.size() > pageSize || m_completionItems.truncated) {
          entries.push_back(
              {.label = m_completionItems.truncated ? "More matches; type to narrow" : "Arrow keys for more matches",
               .enabled = false}
          );
        }
        if (!m_completionItems.error.empty() && entries.empty()) {
          entries.push_back({.label = "Folder suggestions unavailable", .enabled = false});
        }
        const bool visible = m_editingLocation && !entries.empty() && !m_menuSurface;
        m_completionFrame->setVisible(visible);
        m_completionControl->setMaxVisible(entries.size());
        m_completionControl->setEntries(std::move(entries));
        if (m_completionIndex) {
          m_completionControl->setHighlightedIndex(*m_completionIndex - page);
        } else {
          m_completionControl->setHighlightedIndex(m_completionControl->entryCount());
        }
      }
      if (!m_completionFrame->visible()) {
        return;
      }
      const auto anchor = sceneRect(*m_locationArea);
      const float width = std::min(anchor.width, static_cast<float>(m_surface->width()) - 16.0F);
      float x = 0, y = 0;
      // The dropdown extends outside the toolbar's hit bounds.
      (void)Node::mapFromScene(
          m_toolbar, std::clamp(anchor.x, 8.0F, std::max(8.0F, static_cast<float>(m_surface->width()) - width - 8.0F)),
          anchor.y + anchor.height + 4.0F, x, y
      );
      m_completionControl->setMenuWidth(width);
      m_completionFrame->arrange(renderer, {x, y, width, m_completionControl->preferredHeight()});
    }
    void beginLocationEdit() {
      m_blankLocationClick.reset();
      m_lastBreadcrumb.reset();
      m_breadcrumbEditTarget.reset();
      clearCompletions();
      m_editingLocation = true;
      m_locationInvalid = false;
      m_location->setValue(
          browser().model().snapshot().location ? files::locationEditText(*browser().model().snapshot().location)
                                                : std::string{}
      );
      m_location->setVisible(true);
      m_location->setParticipatesInLayout(true);
      m_breadcrumbScroll->setVisible(false);
      m_breadcrumbScroll->setParticipatesInLayout(false);
      m_dispatcher.setFocus(m_location->inputArea());
      m_location->selectAll();
      m_browserDirty = true;
      m_surface->requestUpdate();
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
      m_surface->requestUpdate();
    }
    void breadcrumbClick(const files::Location& target, const InputArea::PointerData* pointer = nullptr) {
      m_lastBreadcrumb = pointer != nullptr ? std::optional(target) : std::nullopt;
      if (pointer != nullptr) {
        m_breadcrumbClickTime = std::chrono::steady_clock::now();
        m_breadcrumbClickX = pointer->sceneX;
        m_breadcrumbClickY = pointer->sceneY;
      }
      browser().open(target);
      m_dispatcher.setFocus(m_list->focusArea());
    }
    void openBreadcrumbMenu(const files::Location& target, LayoutRect anchor) {
      const auto context = browser().contextSerial();
      auto targets = std::make_shared<std::vector<files::Location>>();
      std::vector<ContextMenuControlEntry> entries;
      for (const auto& crumb : files::locationBreadcrumbs(target)) {
        const auto index = targets->size();
        targets->push_back(crumb.location);
        entries.push_back({.id = static_cast<int>(index), .label = crumb.label});
      }
      entries.push_back({.label = "", .separator = true});
      const auto request = actionRequest(false);
      for (const auto action : {Action::CopyPath, Action::CopyUri, Action::Terminal}) {
        auto entry = actionEntry(action, request);
        entry.id = -1 - static_cast<int>(action);
        if (action == Action::Terminal) {
          entry.enabled = target.nativePath().has_value() && m_terminal.available();
        }
        entries.push_back(std::move(entry));
      }
      entries.push_back({.label = "", .separator = true});
      entries.push_back({.label = "Sibling folders", .header = true});
      entries.push_back({.label = "Loading folders…", .enabled = false});
      const auto activate = [this, context, target, targets](int id) {
        if (context != browser().contextSerial()) {
          return;
        }
        if (id >= 0) {
          if (static_cast<std::size_t>(id) < targets->size()) {
            breadcrumbClick((*targets)[static_cast<std::size_t>(id)]);
          }
          return;
        }
        const auto action = static_cast<Action>(-1 - id);
        if (action == Action::Terminal) {
          if (!target.nativePath() || !m_terminal.available()) {
            return;
          }
          const std::weak_ptr<int> lifetime = m_lifetime;
          m_terminal.open(target, [this, lifetime, context](std::string error) {
            if (!lifetime.expired() && context == browser().contextSerial() && !error.empty()) {
              actionError(std::move(error));
            }
          });
        } else if (m_clipboard.available()) {
          m_clipboard.setClipboardText(action == Action::CopyUri ? target.uri() : files::locationEditText(target));
        }
      };
      showContextMenu(entries, activate, anchor);
      const auto generation = m_menuGeneration;
      const files::GioPtr<GFile> parent(g_file_get_parent(target.file().get()));
      const files::GString uri(parent ? g_file_get_uri(parent.get()) : nullptr);
      const auto siblingParent = uri ? files::Location::fromUri(uri.get()) : target;
      m_siblingCompletion.children(
          siblingParent, browser().model().filter().showHidden,
          [this, context, generation, anchor, targets, entries = std::move(entries),
           activate](const files::PathSuggestions& suggestions) mutable {
            if (context != browser().contextSerial() || generation != m_menuGeneration || !m_menuSurface) {
              return;
            }
            entries.pop_back();
            for (const auto& item : suggestions.items) {
              entries.push_back({.id = static_cast<int>(targets->size()), .label = item.label});
              targets->push_back(item.location);
            }
            if (suggestions.items.empty()) {
              entries.push_back(
                  {.label = suggestions.error.empty() ? "No sibling folders" : "Folders unavailable", .enabled = false}
              );
            }
            if (suggestions.truncated) {
              entries.push_back({.label = "First 256 folders; open parent for all", .enabled = false});
            }
            showContextMenu(std::move(entries), activate, anchor);
          }
      );
    }
    void openBreadcrumbOverflow() {
      if (!m_breadcrumbOverflow || !m_breadcrumbOverflow->visible() || m_firstBreadcrumb == 0) {
        return;
      }
      m_lastBreadcrumb.reset();
      m_blankLocationClick.reset();
      const auto context = browser().contextSerial();
      std::vector<files::Location> targets;
      std::vector<ContextMenuControlEntry> entries;
      for (std::size_t index = 0; index < m_firstBreadcrumb; ++index) {
        const auto& part = m_breadcrumbParts[index];
        targets.push_back(part.location);
        entries.push_back({.id = static_cast<int>(index), .label = part.button->label()->text()});
      }
      showContextMenu(
          std::move(entries),
          [this, context, targets = std::move(targets)](int id) {
            if (context == browser().contextSerial() && id >= 0 && static_cast<std::size_t>(id) < targets.size()) {
              breadcrumbClick(targets[static_cast<std::size_t>(id)]);
            }
          },
          sceneRect(*m_breadcrumbOverflow)
      );
    }
    void layoutBreadcrumbs(Renderer& renderer) {
      if (!m_breadcrumbScroll->visible() || m_breadcrumbParts.empty()) {
        return;
      }
      const float available = m_breadcrumbScroll->contentViewportWidth();
      if (available == m_breadcrumbLayoutWidth
          && renderer.renderScale() == m_breadcrumbRenderScale
          && renderer.textMetricsGeneration() == m_breadcrumbMetricsGeneration) {
        return;
      }

      std::vector<float> widths;
      std::vector<float> separators;
      widths.reserve(m_breadcrumbParts.size());
      separators.reserve(m_breadcrumbParts.size());
      float total = 0;
      for (const auto& part : m_breadcrumbParts) {
        part.button->setMaxWidth(160.0F);
        const float width = part.button->measure(renderer, {}).width;
        const float separator = part.separator ? part.separator->measure(renderer, {}).width : 0.0F;
        widths.push_back(width);
        separators.push_back(separator);
        total += width + separator;
      }
      const bool overflow = total > available && m_breadcrumbParts.size() > 1;
      std::size_t first = 0;
      float leafBudget = std::max(1.0F, available);
      if (overflow) {
        const float overflowWidth = m_breadcrumbOverflow->measure(renderer, {}).width;
        first = m_breadcrumbParts.size() - 1;
        leafBudget = std::max(1.0F, available - overflowWidth - separators.back());
        float used = overflowWidth + separators.back() + std::min(widths.back(), leafBudget);
        while (first > 1 && used + widths[first - 1] + separators[first - 1] <= available) {
          --first;
          used += widths[first] + separators[first];
        }
      }
      auto* focus = m_dispatcher.focusedArea();
      m_breadcrumbOverflow->setVisible(overflow);
      m_breadcrumbOverflow->setParticipatesInLayout(overflow);
      if (!overflow && focus == m_breadcrumbOverflow->inputArea()) {
        m_dispatcher.setFocus(m_breadcrumbParts.back().button->inputArea());
      }
      m_dropBreadcrumbs.clear();
      for (std::size_t index = 0; index < m_breadcrumbParts.size(); ++index) {
        const auto& part = m_breadcrumbParts[index];
        const bool visible = index >= first;
        part.button->setVisible(visible);
        part.button->setParticipatesInLayout(visible);
        if (part.separator) {
          const bool separatorVisible = visible && (index > first || overflow);
          part.separator->setVisible(separatorVisible);
          part.separator->setParticipatesInLayout(separatorVisible);
        }
        if (visible) {
          m_dropBreadcrumbs.emplace_back(part.location, part.button);
        } else if (focus == part.button->inputArea()) {
          m_dispatcher.setFocus(m_breadcrumbOverflow->inputArea());
        }
      }
      m_firstBreadcrumb = first;
      m_breadcrumbParts.back().button->setMaxWidth(std::min(160.0F, leafBudget));
      m_breadcrumbScroll->requestScrollToOffset(0);
      m_breadcrumbScroll->arrange(
          renderer,
          {m_breadcrumbScroll->x(), m_breadcrumbScroll->y(), m_breadcrumbScroll->width(), m_breadcrumbScroll->height()}
      );
      m_breadcrumbLayoutWidth = available;
      m_breadcrumbRenderScale = renderer.renderScale();
      m_breadcrumbMetricsGeneration = renderer.textMetricsGeneration();
      m_dispatcher.syncPointerHover();
      m_refreshExternalDrop = true;
    }
    void rebuildBreadcrumbs() {
      m_dropBreadcrumbs.clear();
      m_breadcrumbParts.clear();
      m_breadcrumbOverflow = nullptr;
      m_breadcrumbLayoutWidth = -1;
      while (!m_breadcrumbs->children().empty()) {
        m_breadcrumbs->removeChild(m_breadcrumbs->children().back().get());
      }
      const auto& location = browser().model().snapshot().location;
      if (!location) {
        return;
      }
      const auto crumbs = files::locationBreadcrumbs(*location);
      m_breadcrumbs->addChild(
          ui::button(
              {.out = &m_breadcrumbOverflow,
               .glyph = "dots",
               .controlHeight = Style::controlHeightSm,
               .variant = ButtonVariant::Ghost,
               .tooltip = "Parent folders",
               .paddingH = Style::spaceSm,
               .onClick = [this] { openBreadcrumbOverflow(); }}
          )
      );
      for (std::size_t i = 0; i < crumbs.size(); ++i) {
        Node* separator = nullptr;
        if (i != 0) {
          separator = m_breadcrumbs->addChild(
              ui::glyph(
                  {.glyph = "chevron-right",
                   .glyphSize = 12.0F,
                   .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
              )
          );
        }
        const auto target = crumbs[i].location;
        auto* partButton = static_cast<Button*>(m_breadcrumbs->addChild(
            ui::button(
                {.text = crumbs[i].label,
                 .fontSize = Style::fontSizeCaption,
                 .controlHeight = Style::controlHeightSm,
                 .contentAlign = ButtonContentAlign::Start,
                 .variant = ButtonVariant::Ghost,
                 .tooltip = crumbs[i].label,
                 .maxWidth = 160.0F,
                 .paddingH = Style::spaceSm,
                 .onClick = [this, target] { breadcrumbClick(target); },
                 .configure =
                     [this, target](Button& button) {
                       button.label()->setUseMarkup(false);
                       button.inputArea()->setAcceptedButtons(
                           InputArea::buttonMask(BTN_LEFT) | InputArea::buttonMask(BTN_RIGHT)
                       );
                       button.inputArea()->setOnClick([this, target](const InputArea::PointerData& pointer) {
                         if (pointer.button == BTN_LEFT) {
                           breadcrumbClick(target, &pointer);
                         } else if (pointer.button == BTN_RIGHT) {
                           openBreadcrumbMenu(target, {pointer.sceneX, pointer.sceneY, 1, 1});
                         }
                       });
                     }}
            )
        ));
        m_breadcrumbParts.push_back({target, partButton, separator});
      }
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
      if (!m_filterInput->composing() && m_filterInput->value() != browser().searchQuery()) {
        m_filterInput->setValue(browser().searchQuery());
      }
      m_dispatcher.setFocus(m_filterInput->inputArea(), preserveTextInput);
      if (selectAll) {
        m_filterInput->selectAll();
      }
      m_browserDirty = true;
      m_surface->requestUpdate();
    }

    void changeNameFilter(const std::string& text) {
      m_filterOpen = true;
      browser().setSearchComposing(m_filterInput->composing());
      if (text == browser().searchQuery()) {
        return;
      }
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      m_viewAnchor.reset();
      m_list->scrollView().requestScrollToOffset(0);
      browser().editSearchQuery(text);
    }

    void closeFilter() {
      m_filterOpen = false;
      changeNameFilter({});
      m_filterOpen = false;
      m_filterInput->setValue({});
      m_dispatcher.setFocus(m_list->focusArea());
      m_browserDirty = true;
      m_surface->requestUpdate();
    }

    static constexpr float kSidebarHandleWidth = 6.0F;

    void setSidebarVisible(bool visible, bool persist = true) {
      if (visible == m_showSidebar) {
        return;
      }
      m_dispatcher.cancelPointerCapture();
      if (m_drag) {
        cancelDrag();
      }
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
      if (persist) {
        m_preferences.saveShowSidebar(visible);
      }
      m_surface->requestLayout();
    }

    int sidebarMaximumWidth() const {
      return std::clamp(
          static_cast<int>(m_surface->width()) - 320 - static_cast<int>(kSidebarHandleWidth + kWorkspaceInset * 3),
          files::kMinSidebarWidth, files::kMaxSidebarWidth
      );
    }

    void resizeSidebar(int width, bool persist) {
      const int bounded = std::clamp(width, files::kMinSidebarWidth, sidebarMaximumWidth());
      if (bounded == m_sidebarWidth) {
        return;
      }
      captureViewAnchor();
      m_sidebarWidth = bounded;
      if (persist) {
        m_preferences.saveSidebarWidth(m_sidebarWidth);
      }
      m_surface->requestLayout();
    }

    void captureViewAnchor() {
      if (m_list == nullptr) {
        return;
      }
      const auto first = m_list->firstVisibleIndex();
      const auto& rows = browser().model().rows();
      if (first && *first < rows.size()) {
        m_viewAnchor = rows[*first];
      }
    }

    void setDetailsColumnVisibility(files::DetailsColumnVisibility visibility) {
      if (visibility == m_columnVisibility) {
        return;
      }
      captureViewAnchor();
      m_dispatcher.cancelPointerCapture();
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      m_columnVisibility = visibility;
      m_preferences.saveDetailsColumns(visibility);
      m_browserDirty = true;
      m_surface->requestUpdate();
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
      if (!m_columnResize) {
        return;
      }
      m_columnWidths = m_columnResize->before;
      m_detailsOffset = m_columnResize->offset;
      m_columnResize.reset();
      m_columnHoverDirty = true;
      m_surface->requestLayout();
    }

    void setDetailsColumnWidths(files::DetailsColumnWidths widths) {
      if (m_columnWidths == widths) {
        return;
      }
      captureViewAnchor();
      m_dispatcher.cancelPointerCapture();
      m_list->cancelPointerGesture();
      m_columnWidths = widths;
      m_preferences.saveColumnWidths(widths);
      m_surface->requestLayout();
    }

    void scrollDetails(float offset) {
      if (m_viewMode != files::ViewMode::List || m_columnResize || m_columnDrag) {
        return;
      }
      const float maximum = std::max(0.0F, m_detailsColumns.contentWidth - m_detailsHeaderViewport->width());
      const float next = std::clamp(offset, 0.0F, maximum);
      if (next == m_detailsOffset) {
        return;
      }
      m_list->cancelPointerGesture();
      m_detailsOffset = next;
      m_columnHoverDirty = true;
      m_surface->requestLayout();
    }

    void layoutDetails(Renderer& renderer) {
      if (m_miller.enabled()) {
        m_detailsScrollHost->setVisible(false);
        m_detailsScrollHost->setParticipatesInLayout(false);
        return;
      }
      const bool list = m_viewMode == files::ViewMode::List;
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
        if (!visible && m_dispatcher.focusedArea() == header->inputArea()) {
          m_dispatcher.setFocus(m_nameHeader->inputArea());
        }
      }
      m_detailsOffset = std::clamp(m_detailsOffset, 0.0F, std::max(0.0F, columns.contentWidth - viewport));
      m_listAdapter.setDetailsColumns(columns);
      m_list->setHorizontalOverflow(list ? columns.contentWidth : 0.0F, list ? m_detailsOffset : 0.0F);
      if (columnsChanged) {
        m_list->notifyDataChanged();
      }
      m_root->arrange(
          renderer, {0, 0, static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height())}
      );
      m_body->arrange(renderer, m_pageBounds);
      m_detailsHeaderRow->arrange(
          renderer, {-m_detailsOffset, 0, std::max(viewport, columns.contentWidth), m_detailsHeaderViewport->height()}
      );
      // Reveal focused headers without moving the user's horizontal position
      // on every frame. Pointer resizing retains its own captured offset.
      auto* focused = m_dispatcher.focusedArea();
      Button* reveal = std::exchange(m_revealDetailsHeader, nullptr);
      if (focused != m_lastDetailsFocus && !m_columnResize && !m_columnDrag) {
        for (std::size_t index = 0; index < 4; ++index) {
          if (widthHeader(index)->inputArea() == focused) {
            reveal = widthHeader(index);
          }
        }
      }
      m_lastDetailsFocus = focused;
      if (list && reveal && reveal->visible()) {
        const auto bounds = sceneRect(*reveal);
        const auto clip = sceneRect(*m_detailsHeaderViewport);
        const float delta = bounds.width > clip.width
            ? (Style::rtl() ? bounds.x + bounds.width - clip.x - clip.width : bounds.x - clip.x)
            : bounds.x < clip.x                             ? bounds.x - clip.x
            : bounds.x + bounds.width > clip.x + clip.width ? bounds.x + bounds.width - clip.x - clip.width
                                                            : 0.0F;
        if (delta != 0.0F) {
          scrollDetails(m_detailsOffset + delta);
        }
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
      if (!m_columnDrag || !m_listHeader->visible() || !m_detailsHeaderViewport->containsScenePoint(x, y)) {
        return std::nullopt;
      }
      const auto source = m_columnDrag->source;
      if (!columnHeader(source)->visible()) {
        return std::nullopt;
      }
      std::vector<std::pair<files::DetailsColumn, LayoutRect>> visible;
      auto left = static_cast<float>(m_surface->width());
      float right = 0;
      for (const auto column : m_columnOrder) {
        auto* header = columnHeader(column);
        if (!header->visible()) {
          continue;
        }
        const auto rect = sceneRect(*header);
        left = std::min(left, rect.x);
        right = std::max(right, rect.x + rect.width);
        if (column != source) {
          visible.emplace_back(column, rect);
        }
      }
      const auto bounds = sceneRect(*columnHeader(source));
      if (visible.empty() || x < left || x > right || y < bounds.y || y > bounds.y + bounds.height) {
        return std::nullopt;
      }
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
      if (!order) {
        return std::nullopt;
      }
      return ColumnDrop{*order, {marker - 1.0F, bounds.y, 2.0F, bounds.height}};
    }

    void cancelColumnDrag() {
      const auto drag = std::exchange(m_columnDrag, std::nullopt);
      if (!drag) {
        return;
      }
      auto* header = columnHeader(drag->source);
      header->setCursorShape(drag->cursor);
      header->setHoverSuppressed(false);
      header->setOpacity(1.0F);
      if (m_dispatcher.focusedArea() == header->inputArea()) {
        m_dispatcher.setFocus(m_list->focusArea());
      }
      m_columnHoverDirty = true;
      m_surface->requestLayout();
    }

    void updateColumnDrag() {
      if (!m_columnDrag) {
        return;
      }
      if (!m_columnDrag->active
          && std::hypot(m_pointerX - m_columnDrag->pressX, m_pointerY - m_columnDrag->pressY) < 6.0F) {
        return;
      }
      m_columnDrag->active = true;
      auto* header = columnHeader(m_columnDrag->source);
      header->setHoverSuppressed(true);
      header->setCursorShape(
          columnDropAt(m_pointerX, m_pointerY) ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING
                                               : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP
      );
      m_surface->requestLayout();
    }

    void finishColumnDrag(float x, float y) {
      const auto drop = columnDropAt(x, y);
      m_dispatcher.cancelPointerCapture();
      cancelColumnDrag();
      if (drop) {
        setDetailsColumnOrder(drop->order);
      }
      m_dispatcher.setFocus(m_list->focusArea());
      m_dispatcher.syncPointerHover();
      m_surface->requestLayout();
    }

    void layoutColumnDrag(Renderer& renderer) {
      const bool active = m_columnDrag && m_columnDrag->active;
      for (const auto column : m_columnOrder) {
        columnHeader(column)->setOpacity(active && m_columnDrag->source == column ? 0.65F : 1.0F);
      }
      const auto drop = active ? columnDropAt(m_pointerX, m_pointerY) : std::nullopt;
      const bool changed = drop && drop->order != m_columnOrder;
      m_columnDropLine->setVisible(changed);
      if (changed) {
        m_columnDropLine->arrange(renderer, drop->marker);
      }
    }

    Button* columnHeader(files::DetailsColumn column) const {
      switch (column) {
      case files::DetailsColumn::Type:
        return m_typeHeader;
      case files::DetailsColumn::Size:
        return m_sizeHeader;
      case files::DetailsColumn::Modified:
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
      if (order == m_columnOrder || !files::validDetailsColumnOrder(order)) {
        return;
      }
      captureViewAnchor();
      m_dispatcher.cancelPointerCapture();
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      m_columnOrder = order;
      m_preferences.saveColumnOrder(order);
      m_browserDirty = true;
      m_surface->requestUpdate();
    }

    void changeSort(files::SortOptions options) {
      if (options == browser().model().sort()) {
        return;
      }
      captureViewAnchor();
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      browser().model().setSort(options);
      for (const auto& column : m_miller.columns()) {
        column->browser->model().setSort(options);
      }
      m_preferences.saveSort(options);
      m_dispatcher.setFocus(m_list->focusArea());
    }

    void sortBy(files::SortField field) {
      auto options = browser().model().sort();
      options.descending = options.field == field && !options.descending;
      options.field = field;
      const auto action = field == files::SortField::Name ? Action::SortName
          : field == files::SortField::Type               ? Action::SortType
          : field == files::SortField::Size               ? Action::SortSize
                                                          : Action::SortModified;
      if (browser().model().sort().field == field) {
        dispatchAction(options.descending ? Action::Descending : Action::Ascending);
      } else {
        dispatchAction(action);
      }
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
      browser().setDefaultViewMode(m_viewMode);
      m_browserDirty = true;
      m_surface->requestUpdate();
    }

    void applyViewMode() {
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      if (m_miller.enabled()) {
        m_regularList->setVisible(false);
        m_regularList->setParticipatesInLayout(false);
        m_regularList->setAdapter(nullptr);
        m_millerView->setVisible(true);
        m_millerView->setParticipatesInLayout(true);
        m_appliedViewMode = m_viewMode;
        return;
      }
      m_list = m_regularList;
      m_millerView->setVisible(false);
      m_millerView->setParticipatesInLayout(false);
      const bool grid = m_viewMode == files::ViewMode::Grid;
      m_list->setAdapter(grid ? &m_gridAdapter : &m_listAdapter);
      m_list->setColumns(grid ? 0 : 1);
      m_list->setMinCellWidth(std::max(88.0F, gridIconSize() + 56.0F));
      m_list->setCellHeight(gridHeight());
      m_list->setColumnGap(grid ? Style::spaceXs : 0.0F);
      m_list->setRowGap(grid ? Style::spaceXs : 2.0F);
      m_appliedViewMode = m_viewMode;
    }

    float gridHeight() const {
      const auto& location = browser().model().snapshot().location;
      const bool context = !browser().searchQuery().empty() || (location && location->isTrash());
      return m_viewMode == files::ViewMode::Grid ? gridIconSize() + (context ? 72.0F : 52.0F)
                                                 : (context ? 52.0F : 36.0F);
    }

    float gridIconSize() const { return static_cast<float>(files::kGridIconSizes[m_gridZoom]); }

    void changeGridZoom(int requested) {
      const auto zoom =
          static_cast<std::size_t>(std::clamp(requested, 0, static_cast<int>(files::kGridIconSizes.size()) - 1));
      if (zoom == m_gridZoom) {
        return;
      }
      captureViewAnchor();
      m_listAdapter.onPointerCancel();
      m_gridAdapter.onPointerCancel();
      m_gridZoom = zoom;
      m_preferences.saveGridIconSize(files::kGridIconSizes[m_gridZoom]);
      browser().setDefaultIconSize(static_cast<int>(gridIconSize()));
      m_browserDirty = true;
      m_surface->requestUpdate();
    }

    bool zoomFromKey(const KeyboardEvent& event) {
      if (m_viewMode != files::ViewMode::Grid || m_list == nullptr) {
        return false;
      }
      int zoom = static_cast<int>(m_gridZoom);
      const bool ctrl = event.modifiers == KeyMod::Ctrl;
      const bool ctrlShift = event.modifiers == (KeyMod::Ctrl | KeyMod::Shift);
      if (!ctrl && !ctrlShift) {
        return false;
      }
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
      if (m_viewMode != files::ViewMode::Grid
          || m_list == nullptr
          || !m_list->visible()
          || m_connection.keyboardModifiers() != KeyMod::Ctrl
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
      auto& model = browser().model();
      if (m_miller.enabled() && event.modifiers == 0 && (event.sym == XKB_KEY_Left || event.sym == XKB_KEY_Right)) {
        const bool next = (event.sym == XKB_KEY_Right) != Style::rtl();
        if (next) {
          if (m_miller.enter()) {
            focusMiller(m_miller.focused());
            m_millerFocusPending = true;
          }
        } else if (m_miller.focused() > 0) {
          focusMiller(m_miller.focused() - 1);
          m_dispatcher.setFocus(m_list->focusArea());
        }
        return true;
      }
      if (event.sym == XKB_KEY_a && event.modifiers == KeyMod::Ctrl) {
        dispatchAction(Action::SelectAll);
        return true;
      }
      if (event.modifiers == 0 && KeySymbol::isEscape(event.sym)) {
        dispatchAction(Action::ClearSelection);
        return true;
      }
      if ((event.sym == XKB_KEY_i || event.sym == XKB_KEY_I) && event.modifiers == (KeyMod::Ctrl | KeyMod::Shift)) {
        if (!event.repeat) {
          dispatchAction(Action::InvertSelection);
        }
        return true;
      }
      if (event.sym == XKB_KEY_s && event.modifiers == KeyMod::Ctrl) {
        if (!event.repeat) {
          dispatchAction(Action::SelectPattern);
        }
        return true;
      }
      if (event.modifiers == 0 && KeySymbol::isEnter(event.sym)) {
        if (!event.repeat
            && model.selection().focused
            && (m_fileKeyboardFocus || model.selection().selected.contains(*model.selection().focused))) {
          auto request = actionRequest();
          if (request.targets.empty()) {
            const auto* entry = model.entry(*model.selection().focused);
            if (entry) {
              request.targets.push_back({entry->id, entry->location});
            }
          }
          dispatchAction(Action::Open, request);
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
      } else if (m_viewMode == files::ViewMode::Grid && event.sym == XKB_KEY_Right) {
        index += hasFocus ? (Style::rtl() ? -1 : 1) : 0;
      } else if (m_viewMode == files::ViewMode::Grid && event.sym == XKB_KEY_Left) {
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
        if (m_miller.enabled()) {
          m_miller.interact(m_miller.focused());
        }
        (void)model.select(
            id,
            shift ? (ctrl ? files::SelectionMode::AddRange : files::SelectionMode::Range)
                  : files::SelectionMode::Replace
        );
      }
      m_keyboardReveal = id;
      return true;
    }

    void syncBrowser() {
      if (m_permissionEditor) {
        m_permissionSave->setEnabled(m_permissionEditor->canApply() && !actionContext().operationsBusy);
      }
      if (m_typeaheadContext != browser().contextSerial() || !browser().model().selectionAvailable()) {
        clearTypeahead();
      }
      const auto& model = browser().model();
      const auto& snapshot = model.snapshot();
      if (m_visitSerial != browser().visitSerial()) {
        m_visitSerial = browser().visitSerial();
        const auto& viewport = browser().viewport();
        m_viewMode = m_miller.enabled() ? files::ViewMode::Miller : viewport.mode;
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
      if (auto restored = browser().takeRestoredViewport()) {
        m_pendingHistoryViewport = restored;
        m_viewAnchor.reset();
      }
      m_location->setInvalid(m_locationInvalid);
      if (!m_filterInput->composing() && m_filterInput->value() != browser().searchQuery()) {
        m_filterInput->setValue(browser().searchQuery());
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
      m_nameHeader->glyph()->setVisible(sort.field == files::SortField::Name);
      m_nameHeader->glyph()->setParticipatesInLayout(sort.field == files::SortField::Name);
      m_sizeHeader->glyph()->setVisible(sort.field == files::SortField::Size);
      m_sizeHeader->glyph()->setParticipatesInLayout(sort.field == files::SortField::Size);
      m_modifiedHeader->glyph()->setVisible(sort.field == files::SortField::Modified);
      m_modifiedHeader->glyph()->setParticipatesInLayout(sort.field == files::SortField::Modified);
      m_typeHeader->glyph()->setVisible(sort.field == files::SortField::Type);
      m_typeHeader->glyph()->setParticipatesInLayout(sort.field == files::SortField::Type);
      const char* field = sort.field == files::SortField::Name ? "Name"
          : sort.field == files::SortField::Size               ? "Size"
          : sort.field == files::SortField::Type               ? "Type"
                                                               : "Modified";
      const char* direction = sort.field == files::SortField::Modified
          ? (sort.descending ? "newest first" : "oldest first")
          : sort.field == files::SortField::Size ? (sort.descending ? "largest first" : "smallest first")
                                                 : (sort.descending ? "Z to A" : "A to Z");
      m_menuButton->setTooltip(
          std::format("View options: {}, {}{}", field, direction, sort.directoriesFirst ? " · Folders first" : "")
      );
      const bool grid = m_viewMode == files::ViewMode::Grid;
      m_viewSwitch->setSelectedIndex(grid ? 0 : m_viewMode == files::ViewMode::List ? 1 : 2);
      m_listHeader->setVisible(m_viewMode == files::ViewMode::List);
      m_listHeader->setParticipatesInLayout(m_viewMode == files::ViewMode::List);
      const auto request = actionRequest(false);
      for (const auto& [button, action] : std::array{
               std::pair{m_back, Action::Back}, std::pair{m_forward, Action::Forward}, std::pair{m_up, Action::Up}
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
      const bool loading = snapshot.status == files::DirectoryStatus::Loading;
      m_refresh->setGlyph(loading ? "square" : "refresh");
      m_refresh->setTooltip(loading ? "Stop loading" : "Refresh (F5)");
      std::string status;
      if (m_creator.busy()) {
        const char* kind = m_creator.snapshot().kind == files::CreateKind::Folder ? "folder"
            : m_creator.snapshot().kind == files::CreateKind::Document            ? "document"
            : m_creator.snapshot().kind == files::CreateKind::SymbolicLink
                || m_creator.snapshot().kind == files::CreateKind::HardLink
            ? "link"
            : "file";
        status = m_creator.snapshot().status == files::CreateStatus::Creating
            ? std::format("Creating {}…", kind)
            : std::format("Preparing {} creation…", kind);
      } else if (m_permissions.busy()) {
        status = m_permissions.snapshot().phase == files::PermissionPhase::Saving ? "Saving permissions…"
                                                                                  : "Reading permissions…";
      } else if (m_batchRenamer.busy()) {
        status = m_batchRenamer.applying() ? std::format("Renaming… · {} items", m_batchRenamer.completed())
                                           : "Checking rename preview…";
      } else if (m_renamer.busy()) {
        status = m_renamer.snapshot().status == files::RenameStatus::Renaming ? "Renaming…" : "Preparing rename…";
      } else if (m_restore.busy()) {
        status = std::format("Restoring… · {} of {} items", m_restore.completed(), m_restore.total());
      } else if (m_emptyTrash.busy()) {
        status = m_emptyTrash.snapshot().status == files::EmptyTrashStatus::Preparing ? "Reading Trash…"
            : m_emptyTrash.snapshot().status == files::EmptyTrashStatus::Ready
            ? "Calculating Trash size…"
            : std::format(
                  "{}… · {} of {} items", m_selectedTrashDelete ? "Deleting from Trash" : "Emptying Trash",
                  m_emptyTrash.completed(), m_emptyTrash.snapshot().results.size()
              );
      } else if (m_trash.busy()) {
        status = m_trash.snapshot().status == files::TrashStatus::Preparing
            ? "Preparing move to Trash…"
            : std::format("Moving to Trash… · {} of {} items", m_trash.completed(), m_trash.snapshot().results.size());
      } else if (m_archives.busy()) {
        const files::GString size(g_format_size_full(m_archives.bytes(), G_FORMAT_SIZE_IEC_UNITS));
        const char* phase = m_archives.phase() == files::ArchivePhase::Reading ? "Reading archive"
            : m_archives.phase() == files::ArchivePhase::Compressing           ? "Compressing"
            : m_archives.phase() == files::ArchivePhase::Cleaning              ? "Cleaning temporary files"
            : m_archives.phase() == files::ArchivePhase::Transferring          ? "Publishing archive output"
                                                                               : "Extracting archive";
        status = std::format("{}… · {} entries · {}", phase, m_archives.items(), size.get());
      } else if (m_transfers.busy()) {
        const files::GString size(g_format_size(m_transfers.bytes()));
        status = std::format(
            "{} {} of {} items · {}", m_transfers.action() == files::TransferAction::Copy ? "Copying" : "Moving",
            m_transfers.completed(), m_transfers.total(), size.get()
        );
      } else if (!m_actionError.empty()) {
        status = m_actionError;
      } else if (!m_pins.error().empty()) {
        status = std::format("Could not update pins: {}", m_pins.error());
      } else if (!m_preferenceError.empty()) {
        status = m_preferenceError;
      } else if (!browser().message().empty()) {
        status = browser().message();
      } else if (snapshot.error) {
        status = snapshot.error->message;
      } else if (!browser().searchQuery().empty()) {
        status = std::format(
            "{}{} results",
            loading                                                    ? "Searching… · "
                : snapshot.status == files::DirectoryStatus::Cancelled ? "Search stopped · "
                                                                       : "",
            model.rows().size()
        );
        if (browser().skippedSearchFolders() != 0) {
          status += std::format(" · {} folders could not be searched", browser().skippedSearchFolders());
        }
      } else if (loading) {
        status = "Loading folder…";
      } else if (snapshot.status == files::DirectoryStatus::Cancelled) {
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
      if (snapshot.location && snapshot.location->isTrash() && !snapshot.error && !loading) {
        status += " · Refresh to update";
      }
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
      if (!m_typeahead.text().empty() || !m_typeaheadPreedit.empty()) {
        status = "Select: " + m_typeahead.text() + m_typeaheadPreedit + " · " + status;
      }
      m_status->setText(status);
      const auto freeBytes = snapshot.metadata.filesystem.freeBytes;
      m_freeSpaceAvailable = freeBytes
          && !snapshot.metadataLoading
          && !snapshot.filesystemError
          && !snapshot.error
          && snapshot.location
          && !snapshot.location->isTrash();
      if (m_freeSpaceAvailable) {
        const files::GString size(g_format_size_full(*freeBytes, G_FORMAT_SIZE_IEC_UNITS));
        m_freeSpace->setText(std::format("{} free", size.get()));
      }
      const bool cancellable = m_batchRenamer.applying()
          || m_archives.busy()
          || m_transfers.busy()
          || m_restore.busy()
          || m_trash.busy()
          || m_emptyTrash.busy();
      m_cancelOperation->setVisible(cancellable);
      m_cancelOperation->setParticipatesInLayout(cancellable);
      m_cancelOperation->setTooltip(
          m_batchRenamer.applying() ? "Cancel batch rename"
              : m_restore.busy()    ? "Cancel restoration"
              : m_emptyTrash.busy() ? (m_selectedTrashDelete ? "Cancel deletion" : "Cancel emptying Trash")
              : m_trash.busy()      ? "Cancel move to Trash"
                                    : "Cancel transfer"
      );
      m_notice->setText(
          snapshot.error
                  || loading
                  || snapshot.status == files::DirectoryStatus::Cancelled
                  || !browser().message().empty()
                  || !m_preferenceError.empty()
              ? status
              : !browser().searchQuery().empty() ? "No matching files or folders."
              : snapshot.entries.empty()
              ? (snapshot.location == files::Location::trash() ? "Trash is empty." : "This folder is empty.")
              : "No visible items."
      );
      const bool empty = model.rows().empty();
      m_noticeArea->setVisible(empty && !m_miller.enabled());
      m_noticeArea->setParticipatesInLayout(empty && !m_miller.enabled());
      if (!m_miller.enabled()) {
        m_list->setVisible(!empty);
        m_list->setParticipatesInLayout(!empty);
      }
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
        if (found != model.rows().end()) {
          m_list->scrollToIndex(static_cast<std::size_t>(found - model.rows().begin()));
        }
      }
      if (!m_batchReveal.empty() && snapshot.status != files::DirectoryStatus::Loading) {
        if (m_batchRevealContext == browser().contextSerial()) {
          std::unordered_set<std::string> uris;
          for (const auto& target : m_batchReveal) {
            uris.insert(target.uri());
          }
          std::vector<files::EntryId> ids;
          for (const auto id : model.rows()) {
            if (const auto* entry = model.entry(id); entry && uris.contains(entry->location.uri())) {
              ids.push_back(id);
            }
          }
          browser().model().replaceSelection(ids);
          if (!ids.empty()) {
            const auto found = std::ranges::find(model.rows(), ids.front());
            m_list->scrollToIndex(static_cast<std::size_t>(found - model.rows().begin()));
          }
        }
        // Staging emits several monitor events. Keep reconciling while the results
        // are open, then leave the final selection under normal user control.
        if (!m_batchRenameDialog || m_batchRevealContext != browser().contextSerial()) {
          m_batchReveal.clear();
        }
      }
      if (m_revealTarget && snapshot.status != files::DirectoryStatus::Loading) {
        const auto target = *std::exchange(m_revealTarget, std::nullopt);
        const auto found = std::ranges::find_if(model.rows(), [&](files::EntryId id) {
          const auto* entry = model.entry(id);
          return entry != nullptr && entry->location == target;
        });
        if (found != model.rows().end()) {
          browser().model().select(*found);
          m_list->scrollToIndex(static_cast<std::size_t>(found - model.rows().begin()));
        }
      }
    }

    std::uint32_t resizeEdges(float x, float y) const {
      const bool left = x<6, right = x> static_cast<float>(m_surface->width()) - 6;
      const bool top = y<6, bottom = y> static_cast<float>(m_surface->height()) - 6;
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
      if (!m_renderer.makeCurrent(m_surface->renderTarget())) {
        return;
      }
      auto& renderer = m_surface->renderTarget().renderer();
      UiPhaseScope update(UiPhase::Update);
      if (m_root == nullptr) {
        layout = true;
        UiPhaseScope phase(UiPhase::Layout);
        buildScene();
      }
      if (std::exchange(m_restartMiller, false) && m_miller.enabled()) {
        m_browser.duplicateFrom(m_miller.active());
        m_miller.clear();
        m_list = m_regularList;
      }
      if (m_viewMode == files::ViewMode::Miller && !m_miller.enabled()) {
        m_miller.start(m_browser, m_savedSettings.millerShowParents);
        if (m_miller.enabled()) {
          m_browser.stop();
        }
      } else if (m_viewMode != files::ViewMode::Miller && m_miller.enabled()) {
        m_browser.duplicateFrom(m_miller.active());
        m_browser.setDefaultViewMode(m_viewMode);
        m_miller.clear();
        m_list = m_regularList;
        m_millerFocusPending = true;
      }
      if (m_visible) {
        m_millerView->loadVisible();
      }
      m_miller.synchronize();
      if (m_browserDirty || m_appliedViewMode != m_viewMode) {
        const bool restoreFocus = m_miller.enabled()
            && m_lastBrowser != &browser()
            && m_list
            && m_dispatcher.focusedArea() == m_list->focusArea();
        UiPhaseScope phase(UiPhase::Layout);
        m_millerView->activationMode = m_activationMode;
        m_millerView->synchronize();
        if (auto* grid = m_millerView->focusedView()) {
          m_list = grid;
        }
        if (m_lastBrowser != &browser()) {
          m_lastBrowser = &browser();
          m_visitSerial = 0;
          m_keyboardReveal.reset();
          m_pendingHistoryViewport.reset();
          m_thumbnailContext = 0;
          clearTypeahead();
        }
        m_millerFocusPending = m_millerFocusPending || restoreFocus;
      }
      if (std::exchange(m_millerFocusPending, false)) {
        m_millerView->revealFocused();
        m_dispatcher.setFocus(m_list->focusArea());
      }
      if (m_miller.enabled() && m_browserDirty) {
        for (const auto& column : m_miller.columns()) {
          m_thumbnails.synchronizeSources(column->browser->model().snapshot());
        }
      }
      const auto& thumbnailSnapshot = browser().model().snapshot();
      if (m_thumbnailContext != browser().contextSerial() || thumbnailSnapshot.generation != m_thumbnailRevision) {
        m_thumbnailRevision = thumbnailSnapshot.generation;
        m_thumbnailContext = browser().contextSerial();
        m_thumbnails.synchronizeSources(thumbnailSnapshot);
      }
      if (std::exchange(m_settingsDirty, false)) {
        if (m_pendingSettings) {
          applySettings(m_pendingSettings->browser, !m_preferenceError.empty());
        }
        if (m_settingsView) {
          const auto& inherited = m_preferences.inheritedSettings();
          m_settingsView->sync(
              m_savedSettings, inherited ? inherited->browser : files::BrowserSettings{},
              m_preferences.busy() || m_thumbnails.clearing(),
              m_preferenceError.empty() ? m_thumbnails.error() : m_preferenceError, m_thumbnails.clearing()
          );
          m_surface->requestLayout();
        }
      }
      m_sidebarResizeLine->setFill(colorSpecFromRole(
          m_resizingSidebar || m_sidebarResize->focused() ? ColorRole::Primary
              : m_sidebarResize->hovered()                ? ColorRole::OnSurfaceVariant
                                                          : ColorRole::OutlineVariant
      ));
      m_sidebarResizeLine->setVisible(m_resizingSidebar || m_sidebarResize->focused() || m_sidebarResize->hovered());
      if (std::exchange(m_archiveDirty, false)) {
        syncArchive();
      }
      syncTransferConflict();
      if (std::exchange(m_trashDirty, false)) {
        syncTrash();
      }
      if (std::exchange(m_emptyTrashDirty, false)) {
        syncEmptyTrash();
      }
      if (std::exchange(m_creationDirty, false)) {
        syncCreation();
      }
      if (std::exchange(m_permissionDirty, false)) {
        syncPermissions();
      }
      if (std::exchange(m_batchRenameDirty, false)) {
        syncBatchRename();
      }
      if (std::exchange(m_renameDirty, false)) {
        syncFileRename();
      }
      if (m_browserDirty) {
        m_browserDirty = false;
        syncPlaceRename();
        syncBrowser();
        layout = true;
      }
      if (const auto target = std::exchange(m_breadcrumbEditTarget, std::nullopt);
          target && target == browser().model().snapshot().location) {
        beginLocationEdit();
        layout = true;
      }
      if (layout) {
        UiPhaseScope phase(UiPhase::Layout);
        const bool showFreeSpace = m_freeSpaceAvailable && m_pageBounds.width >= kMinimumFreeSpacePaneWidth;
        m_freeSpace->setVisible(showFreeSpace);
        m_freeSpace->setParticipatesInLayout(showFreeSpace);
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
        if (m_externalScrollDelta != 0) {
          const auto delta = std::exchange(m_externalScrollDelta, 0.0F);
          if (m_externalScrollInPlaces) {
            m_places->dragScroll(delta);
          } else {
            auto& scroll = dragView()->scrollView();
            scroll.requestScrollToOffset(scroll.scrollOffset() + delta);
          }
          m_refreshExternalDrop = true;
        }
        if (m_drag && m_dragScrollDelta != 0) {
          const auto delta = std::exchange(m_dragScrollDelta, 0.0F);
          if (m_dragScrollInPlaces) {
            m_places->dragScroll(delta);
          } else if (m_miller.enabled()) {
            auto& scroll = dragView()->scrollView();
            scroll.requestScrollToOffset(scroll.scrollOffset() + delta);
          } else {
            m_list->scrollDuringDrag(delta);
          }
        }
        const float searchWidth = std::clamp(static_cast<float>(m_surface->width()) * 0.22F, 148.0F, 220.0F);
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
          const auto& rows = browser().model().rows();
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
          const auto& rows = browser().model().rows();
          const auto found = viewport.first ? std::ranges::find(rows, *viewport.first) : rows.end();
          if (found != rows.end()) {
            m_list->scrollToIndex(static_cast<std::size_t>(found - rows.begin()), true, viewport.withinRow);
          } else {
            m_list->scrollView().requestScrollToOffset(viewport.vertical);
          }
        }
        const bool clientHeader = false;
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
            renderer, LayoutRect{0, 0, static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height())}
        );
        m_body->arrange(renderer, m_pageBounds);
        if (m_settingsView) {
          m_settingsView->setCompact(m_surface->width() < 800);
          m_settingsView->arrange(renderer, m_pageBounds);
        }
        if (m_archiveView) {
          m_archiveView->arrange(renderer, m_pageBounds);
        }
        layoutDetails(renderer);
        const bool columnHoverDirty = std::exchange(m_columnHoverDirty, false);
        if (columnOrderChanged || columnHoverDirty) {
          m_dispatcher.syncPointerHover();
        }
        layoutColumnDrag(renderer);
        if (std::exchange(m_refreshExternalDrop, false)) {
          m_clipboard.refreshDrag();
        }
        m_sidebarResizeLine->arrange(renderer, {2.5F, 0, 1.0F, m_sidebarResize->height()});
        layoutDrag(renderer);
        layoutCompletions(renderer);
        m_thumbnails.endFrame();
        if (placesFocus) {
          m_dispatcher.restoreTabFocus(std::move(*placesFocus));
        }
        if (m_restorePlaceFocus && m_menuSurface == nullptr && !m_settingsView && !m_archiveView) {
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
                      m_surface->beginMove(data.serial);
                    } else {
                      m_surface->showWindowMenu(
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
          ui::button({.glyph = "minus", .variant = ButtonVariant::Ghost, .onClick = [this] { m_surface->minimize(); }}),
          ui::button(
              {.glyph = "square", .variant = ButtonVariant::Ghost, .onClick = [this] { m_surface->toggleMaximized(); }}
          ),
          ui::button({.glyph = "x", .variant = ButtonVariant::Ghost, .onClick = [this] { m_surface->close(); }})
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
                     m_locationInvalid = !browser().openText(text);
                     if (!m_locationInvalid) {
                       endLocationEdit(true);
                     }
                     m_browserDirty = true;
                     m_surface->requestUpdate();
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
                     if (m_filterInput->composing()) {
                       return;
                     }
                     browser().submitSearchQuery();
                     if (!browser().model().rows().empty()) {
                       m_dispatcher.setFocus(m_list->focusArea());
                     }
                   },
               .configure =
                   [this](Input& input) {
                     input.setOnCompositionChange([this](bool composing) { browser().setSearchComposing(composing); });
                     input.setClearButtonWidth(24.0F);
                     input.inputArea()->setTooltip("Search names in this folder and all subfolders");
                     input.setOnFocusGain([this] {
                       m_filterOpen = true;
                       m_browserDirty = true;
                       m_surface->requestUpdate();
                     });
                     input.setOnFocusLoss([this] {
                       m_browserDirty = true;
                       m_surface->requestUpdate();
                     });
                   }}
          )
      );
      auto toolbar = ui::row(
          {.out = &m_toolbar, .align = FlexAlign::Center, .gap = Style::spaceSm, .padding = Style::spaceSm},
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceXs},
              ui::button(
                  {.out = &m_back,
                   .glyph = "arrow-left",
                   .variant = ButtonVariant::Ghost,
                   .tooltip = "Back (Alt+Left)",
                   .onClick = [this] { dispatchAction(Action::Back); }}
              ),
              ui::button(
                  {.out = &m_forward,
                   .glyph = "arrow-right",
                   .variant = ButtonVariant::Ghost,
                   .tooltip = "Forward (Alt+Right)",
                   .onClick = [this] { dispatchAction(Action::Forward); }}
              ),
              ui::button(
                  {.out = &m_up,
                   .glyph = "arrow-up",
                   .variant = ButtonVariant::Ghost,
                   .tooltip = "Parent folder (Alt+Up)",
                   .onClick = [this] { dispatchAction(Action::Up); }}
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
                         if (browser().model().snapshot().status == files::DirectoryStatus::Loading) {
                           dispatchAction(Action::Stop);
                         } else {
                           dispatchAction(Action::Refresh);
                         }
                       }}
              ),
              ui::segmented(
                  {.out = &m_viewSwitch,
                   .options =
                       std::vector<ui::SegmentedOption>{
                           {.label = "", .glyph = "layout-grid", .tooltip = "Grid view (Ctrl+1)"},
                           {.label = "", .glyph = "list", .tooltip = "List view (Ctrl+2)"},
                           {.label = "", .glyph = "columns-3", .tooltip = "Column view (Ctrl+3)"}
                       },
                   .selectedIndex = m_viewMode == files::ViewMode::Grid ? 0U : m_viewMode == files::ViewMode::List ? 1U : 2U,
                   .compact = true,
                   .onChange = [this](std::size_t index) { dispatchAction(index == 0 ? Action::Grid : index == 1 ? Action::List : Action::Miller); }}
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
      configurePlacesCallbacks();
      auto sidebarResize = ui::inputArea(
          {.out = &m_sidebarResize,
           .acceptedButtons = InputArea::buttonMask(BTN_LEFT),
           .cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE,
           .focusable = true,
           .tabStop = true,
           .tooltip = "Resize Places; Left/Right adjusts, Home resets",
           .width = kSidebarHandleWidth,
           .onEnter = [this](const auto&) { m_surface->requestUpdate(); },
           .onLeave = [this] { m_surface->requestUpdate(); },
           .onMotion =
               [this](const InputArea::PointerData& pointer) {
                 if (!m_resizingSidebar) {
                   return;
                 }
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
                 m_surface->requestUpdate();
               },
           .onKeyDown =
               [this](const InputArea::KeyData& key) {
                 if (!key.pressed || key.preedit || (key.modifiers != 0 && key.modifiers != KeyMod::Shift)) {
                   return;
                 }
                 const int step = key.modifiers == KeyMod::Shift ? 24 : 8;
                 if (KeySymbol::isLeft(key.sym) || KeySymbol::isRight(key.sym)) {
                   const int direction = KeySymbol::isRight(key.sym) != Style::rtl() ? 1 : -1;
                   resizeSidebar(static_cast<int>(std::lround(m_places->width())) + direction * step, true);
                 } else if (KeySymbol::isHome(key.sym) && key.modifiers == 0) {
                   resizeSidebar(files::kDefaultSidebarWidth, true);
                 }
               },
           .onFocusGain = [this] { m_surface->requestUpdate(); },
           .onFocusLoss = [this] { m_surface->requestUpdate(); },
           .configure =
               [this](InputArea& handle) {
                 handle.setTabFocusKey("places-width");
                 handle.setOnCancel([this] {
                   if (std::exchange(m_resizingSidebar, false)) {
                     m_sidebarWidth = m_sidebarStartWidth;
                     if (m_dispatcher.focusedArea() == m_sidebarResize) {
                       m_dispatcher.setFocus(nullptr);
                     }
                     m_surface->requestLayout();
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
                               .onClick = [this] { sortBy(files::SortField::Name); }}
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
                               .onClick = [this] { sortBy(files::SortField::Type); }}
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
                               .onClick = [this] { sortBy(files::SortField::Size); }}
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
                               .onClick = [this] { sortBy(files::SortField::Modified); }}
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
                   m_surface->requestUpdate();
                 });
                 list.focusArea()->setOnFocusLoss([this] {
                   m_browserDirty = true;
                   m_surface->requestUpdate();
                 });
               }}
          )
      );
      m_regularList = m_list;
      auto miller = std::make_unique<MillerView>(m_miller, m_icons, m_thumbnails);
      m_millerView = miller.get();
      miller->setVisible(false);
      miller->setParticipatesInLayout(false);
      miller->modifiers = [this] { return m_connection.keyboardModifiers(); };
      miller->keyboardFocus = [this] { return m_fileKeyboardFocus && m_list && m_list->focusArea()->focused(); };
      miller->cut = [this](const files::Location& location) { return m_clipboard.isCut(location); };
      miller->focus = [this](std::size_t index) { focusMiller(index); };
      miller->activate = [this](files::EntryId id) {
        auto request = actionRequest(false);
        if (const auto* entry = browser().model().entry(id)) {
          request.targets.push_back({id, entry->location});
        }
        dispatchAction(Action::Open, request);
      };
      miller->context = [this](std::optional<files::EntryId> entry, float x, float y) { openFileMenu(entry, x, y); };
      miller->drag = [this](const std::vector<files::EntryId>& ids) { startFileDrag(ids); };
      miller->cancelDrag = [this] {
        if (!m_systemDrag) {
          cancelDrag();
        }
      };
      miller->resized = [this](int width) {
        m_miller.defaultWidth = static_cast<float>(width);
        m_preferences.submit({{files::BrowserPreference::MillerColumnWidth, width}});
      };
      miller->textInputClient = this;
      miller->requestUpdate = [this] { m_surface->requestUpdate(); };
      content->addChild(std::move(miller));
      auto horizontalBar = std::make_unique<Scrollbar>();
      horizontalBar->setOrientation(ScrollOrientation::Horizontal);
      horizontalBar->setParticipatesInLayout(false);
      horizontalBar->setOnScrollChanged([this](float offset) { scrollDetails(offset); });
      m_detailsScrollbar = horizontalBar.get();
      auto horizontalHost = ui::box(
          {.out = &m_detailsScrollHost,
           .fill = clearColorSpec(),
           .height = Style::scrollbarWidth + Style::scrollbarGap,
           .visible = false,
           .participatesInLayout = false}
      );
      horizontalHost->addChild(std::move(horizontalBar));
      content->addChild(std::move(horizontalHost));
      m_detachedRoot = ui::column(
          {.align = FlexAlign::Stretch, .fill = clearColorSpec(), .fillWidth = true, .fillHeight = true},
          std::move(header), std::move(toolbar),
          ui::column(
              {.out = &m_body,
               .align = FlexAlign::Stretch,
               .fill = colorSpecFromRole(ColorRole::Surface),
               .clipChildren = true,
               .participatesInLayout = false},
              ui::row(
                  {.out = &m_paneHeader,
                   .align = FlexAlign::Center,
                   .gap = Style::spaceXs,
                   .paddingH = Style::spaceSm,
                   .height = 32.0F,
                   .visible = false,
                   .participatesInLayout = false},
                  ui::label(
                      {.out = &m_paneTitle,
                       .fontSize = Style::fontSizeCaption,
                       .maxLines = 1,
                       .flexGrow = 1,
                       .configure = [](Label& label) { label.setUseMarkup(false); }}
                  ),
                  ui::button(
                      {.glyph = "x",
                       .glyphSize = Style::baseGlyphSize,
                       .controlHeight = 24.0F,
                       .variant = ButtonVariant::Ghost,
                       .tooltip = "Close pane (Ctrl+Alt+W)",
                       .padding = Style::spaceXs,
                       .width = 24.0F,
                       .onClick =
                           [this] {
                             if (m_workspaceRequest) {
                               m_workspaceRequest(WorkspaceCommand::ClosePane, std::nullopt);
                             }
                           }}
                  )
              ),
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
                         m_batchRenamer.cancel();
                         m_archives.cancel();
                         m_transfers.cancel();
                         m_restore.cancel();
                         m_trash.cancel();
                         m_emptyTrash.cancel();
                       }}
                  )
              )
          )
      );
      m_root = m_detachedRoot.get();
      m_detachedSidebar = ui::row(
          {.align = FlexAlign::Stretch}, m_places->parent()->removeChild(m_places),
          m_sidebarResize->parent()->removeChild(m_sidebarResize)
      );
      m_sidebarBundle = m_detachedSidebar.get();
      m_sidebarBundle->setAnimationManager(&m_animations);
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
            if (m_columnWidths != before) {
              m_preferences.saveColumnWidths(m_columnWidths);
            }
            m_columnHoverDirty = true;
          }
          m_surface->requestLayout();
        });
        handle->setOnMotion([this](const InputArea::PointerData&) {
          if (!m_columnResize) {
            return;
          }
          const auto& resize = *m_columnResize;
          m_columnWidths = resize.before;
          const int allocation = std::clamp(
              static_cast<int>(std::lround(resize.width + (m_pointerX - resize.pressX) * (Style::rtl() ? -1 : 1))),
              files::minimumDetailsColumnWidth(resize.index), files::maximumDetailsColumnWidth(resize.index)
          );
          if (allocation != static_cast<int>(std::lround(resize.width))) {
            m_columnWidths[resize.index] = allocation;
            if (resize.index != 0 && m_columnWidths[0] == 0) {
              m_columnWidths[0] = resize.nameWidth;
            }
          }
          m_surface->requestLayout();
        });
        handle->setOnCancel([this] { cancelColumnResize(); });
        handle->setOnEnter([this](const auto&) { m_surface->requestLayout(); });
        handle->setOnLeave([this] { m_surface->requestLayout(); });
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
      m_detachedDragOverlay = std::make_unique<Node>();
      m_dragOverlay = m_detachedDragOverlay.get();
      m_dragOverlay->setZIndex(80);
      m_dragOverlay->setParticipatesInLayout(false);
      m_dragOverlay->setHitTestVisible(false);
      m_dragOverlay->setAnimationManager(&m_animations);
      auto dragIcon = std::make_unique<files::FileIcon>(m_icons, 40);
      m_dragIcon = dragIcon.get();
      m_dragIcon->setVisible(false);
      m_dragIcon->setParticipatesInLayout(false);
      m_dragIcon->setOpacity(0.85F);
      m_dragIcon->setZIndex(100);
      m_dragOverlay->addChild(std::move(dragIcon));
      m_dragOverlay->addChild(
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
      m_dragOverlay->addChild(
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
      m_dragOverlay->addChild(
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
      m_toolbar->addChild(std::move(completionFrame));
      m_root->setAnimationManager(&m_animations);
      m_dispatcher.setSceneRoot(m_root);
    }

    void revealMenuHighlight() {
      if (!m_menuScroll || !m_menu) {
        return;
      }
      const auto index = m_menu->highlightedIndex();
      const float top = m_menu->rowTop(index), bottom = m_menu->rowBottom(index);
      const float offset = m_menuScroll->scrollOffset();
      if (top < offset) {
        m_menuScroll->requestScrollToOffset(top);
      } else if (bottom > offset + m_menuScroll->height()) {
        m_menuScroll->requestScrollToOffset(bottom - m_menuScroll->height());
      }
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
      closeDocumentMenu();
      m_documentRow.reset();
      m_quickPreview = nullptr;
      m_appChoiceContainer = nullptr;
      m_appChoiceScroll = nullptr;
      m_launcher.cancelApplications();
      if (!m_previewBounds || !previewVisible()) {
        m_preview.clear();
      }
      ++m_menuGeneration;
      if (std::exchange(m_emptyTrashDialog, false)) {
        m_emptyTrash.cancel();
      }
      m_emptyTrashMessage = nullptr;
      m_emptyTrashSize = nullptr;
      m_emptyTrashConfirm = nullptr;
      if (const auto id = std::exchange(m_activeTransferConflict, std::nullopt)) {
        (void)(m_transferConflictRestore       ? m_restore.resolve(*id, files::TransferConflictChoice::Cancel)
                   : m_transferConflictArchive ? m_archives.resolve(*id, files::TransferConflictChoice::Cancel)
                                               : m_transfers.resolve(*id, files::TransferConflictChoice::Cancel));
      }
      if (m_placeRenameTarget) {
        m_restorePlaceFocus = m_placeRenameTarget->location;
      }
      if (m_fileRenameTarget) {
        m_renamer.cancel();
        if (m_list != nullptr) {
          m_dispatcher.setFocus(m_list->focusArea());
        }
      }
      if (m_permissionEditor) {
        m_permissions.cancel();
        m_permissionEditor = nullptr;
        m_permissionTabs = nullptr;
        m_permissionSave = nullptr;
        m_permissionScroll = nullptr;
        if (m_list) {
          m_dispatcher.setFocus(m_list->focusArea());
        }
      }
      if (m_batchRenameDialog) {
        m_batchRenamer.cancel();
        m_batchRenameDialog = nullptr;
        if (m_list) {
          m_dispatcher.setFocus(m_list->focusArea());
        }
      }
      if (m_createParent || m_linkDialog) {
        m_creator.cancel();
        if (m_list != nullptr) {
          m_dispatcher.setFocus(m_list->focusArea());
        }
      }
      m_createEditor = nullptr;
      m_linkDialog = nullptr;
      m_linkPreparing = false;
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
      if (auto dismiss = std::exchange(m_selectDismiss, {})) {
        dismiss();
      }
      m_aboutScroll = nullptr;
      m_aboutMessage = nullptr;
      m_dialogHeight.reset();
      m_dialogWidth = 588.0F;
      m_dialogPosition.reset();
      m_menuDispatcher.setFocusChangeCallback({});
      m_menuRoot.reset();
      m_menu = nullptr;
      m_menuScroll = nullptr;
      m_menuSurface.reset();
      m_dispatcher.setTextInputContext(m_surface->wlSurface(), &m_textInput);
      m_surface->requestLayout();
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
        m_connection.extension<files::DesktopIntegration>().setCursorShape(serial, shape);
      });
      m_menuSurface->setPrepareFrameCallback([this](bool, bool) {
        if (m_menuSurface == nullptr || !m_renderer.makeCurrent(m_menuSurface->renderTarget())) {
          return;
        }
        UiPhaseScope phase(UiPhase::Layout);
        if (m_dialogHeight) {
          const auto width =
              static_cast<std::uint32_t>(std::min(m_dialogWidth, static_cast<float>(m_surface->width()) - 32.0F));
          LayoutConstraints constraints;
          constraints.setExactWidth(static_cast<float>(width));
          if (m_permissionEditor) {
            m_menuRoot->setSize(
                static_cast<float>(width), std::min(600.0F, static_cast<float>(m_surface->height()) - 64.0F)
            );
          }
          if (m_batchRenameDialog) {
            m_batchRenameDialog->setSize(
                static_cast<float>(width), std::min(560.0F, static_cast<float>(m_surface->height()) - 64.0F)
            );
          }
          m_dialogHeight = m_menuRoot->measure(m_menuSurface->renderTarget().renderer(), constraints).height;
          const auto height = static_cast<std::uint32_t>(
              std::ceil(std::min(*m_dialogHeight, static_cast<float>(m_surface->height()) - 64.0F))
          );
          if (width != m_menuSurface->width() || height != m_menuSurface->height()) {
            (void)m_menuSurface->resize(width, height, false);
          }
          const std::pair<int, int> position{
              static_cast<int>((m_surface->width() - width) / 2), static_cast<int>((m_surface->height() - height) / 2)
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
                  static_cast<float>(m_surface->height()) - 64.0F
              )
          ));
          if (height != m_menuSurface->height()) {
            const auto width = m_menuSurface->width();
            (void)m_menuSurface->resize(width, height, false);
            (void)m_menuSurface->repositionAnchor(
                {.anchorX = static_cast<int>((m_surface->width() - width) / 2),
                 .anchorY = static_cast<int>((m_surface->height() - height) / 2),
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
        if (m_batchRenameDialog) {
          m_batchRenameDialog->syncTooltip(m_menuSurface->renderTarget().renderer());
        }
        if (m_quickPreview) {
          m_quickPreview->sync(
              m_preview.snapshot(), static_cast<float>(m_menuSurface->width()),
              static_cast<float>(m_menuSurface->height()),
              m_preview.snapshot().entry ? browser().model().typeLabel(m_preview.snapshot().entry->id) : "", true
          );
          if (!m_preview.snapshot().image || !m_preview.snapshot().image->pixels) {
            m_quickPreview->clear(m_menuSurface->renderTarget().renderer());
          }
          m_menuRoot->arrange(
              m_menuSurface->renderTarget().renderer(),
              {0, 0, static_cast<float>(m_menuSurface->width()), static_cast<float>(m_menuSurface->height())}
          );
        }
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
          .serial = m_popupSerial != 0 ? m_popupSerial : m_connection.lastInputSerial(),
      };
      if (!m_menuSurface->initializeAsChild(
              m_surface->xdgSurface(), m_connection.outputForSurface(m_surface->wlSurface()), config
          )) {
        closeMenu();
      } else {
        m_menuDispatcher.setTextInputContext(m_menuSurface->wlSurface(), &m_textInput, true, m_surface->wlSurface());
      }
    }

    void showContextMenu(
        std::vector<ContextMenuControlEntry> entries, std::function<void(int)> action, LayoutRect anchor,
        bool alignLeft = false, float preferredWidth = 264.0F
    ) {
      if (m_columnDrag || m_columnResize) {
        m_dispatcher.cancelPointerCapture();
      }
      cancelDrag();
      auto menu = std::make_unique<ContextMenuControl>();
      auto* control = menu.get();
      const float contentHeight = ContextMenuControl::preferredHeight(entries, entries.size(), 1.0F);
      const float height = std::min(contentHeight, std::max(120.0F, static_cast<float>(m_surface->height()) - 32.0F));
      control->setMaxVisible(entries.size());
      const float width = std::min(preferredWidth, static_cast<float>(m_surface->width()) - 32.0F);
      control->setEntries(std::move(entries));
      const bool overflowing = contentHeight > height + 0.5F;
      const float gutter = overflowing ? Style::scrollbarWidth + Style::scrollbarGap : 0.0F;
      // The menu has an intrinsic width; reserve the scroller's lane explicitly.
      control->setMenuWidth(width - gutter);
      const auto activate = [this, action = std::move(action)](const ContextMenuControlEntry& entry) {
        const int id = entry.id;
        const std::weak_ptr<int> lifetime = m_lifetime;
        dismissMenu();
        DeferredCall::callLater([lifetime, action, id] {
          if (!lifetime.expired()) {
            action(id);
          }
        });
      };
      control->setOnActivate(activate);
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
        const char* label = candidate == files::DetailsColumn::Type ? "Type"
            : candidate == files::DetailsColumn::Size               ? "Size"
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
              if (const auto order = movedColumnOrder(*column, id == 5 ? left : -left)) {
                setDetailsColumnOrder(*order);
              }
              m_dispatcher.setFocus(m_list->focusArea());
              return;
            }
            if (id == 7) {
              setDetailsColumnOrder(files::kDefaultDetailsColumnOrder);
              m_dispatcher.setFocus(m_list->focusArea());
              return;
            }
            auto visibility = m_columnVisibility;
            if (id == 1) {
              visibility.type = !visibility.type;
            } else if (id == 2) {
              visibility.size = !visibility.size;
            } else if (id == 3) {
              visibility.modified = !visibility.modified;
            } else if (id == 4) {
              visibility = {};
            }
            setDetailsColumnVisibility(visibility);
            m_dispatcher.setFocus(m_list->focusArea());
          },
          anchor
      );
    }

    void toggleHidden() {
      browser().toggleHidden();
      const bool show = browser().model().filter().showHidden;
      for (const auto& column : m_miller.columns()) {
        if (column->browser->model().filter().showHidden != show) {
          column->browser->toggleHidden();
        }
      }
      m_preferences.saveShowHidden(browser().model().filter().showHidden);
    }

    void showActionMenu(std::initializer_list<Action> actions, bool radio = false, bool back = false) {
      const auto request = actionRequest(false);
      std::vector<ContextMenuControlEntry> entries;
      if (back) {
        entries.push_back(actionEntry(Action::ViewMenu, request));
        entries.push_back({.label = "", .separator = true});
      }
      for (const auto action : actions) {
        entries.push_back(actionEntry(action, request, radio && action != Action::FoldersFirst));
      }
      showContextMenu(
          std::move(entries),
          [this, request](int id) {
            const auto action = static_cast<Action>(id);
            dispatchAction(action, request);
            if (!m_menuSurface && action != Action::Search && action != Action::EditLocation) {
              m_dispatcher.setFocus(m_list->focusArea());
            }
          },
          sceneRect(*m_menuButton), true
      );
    }
    void openSortMenu() {
      showActionMenu(
          {Action::SortName, Action::SortType, Action::SortSize, Action::SortModified, Action::Ascending,
           Action::Descending, Action::FoldersFirst},
          true, true
      );
    }
    void openSelectionMenu() {
      showActionMenu(
          {Action::SelectAll, Action::InvertSelection, Action::SelectPattern, Action::ClearSelection}, false, true
      );
    }
    void openInteractionMenu() {
      const auto request = actionRequest(false);
      std::vector<ContextMenuControlEntry> entries{
          actionEntry(Action::ViewMenu, request),
          {.label = "Open items with", .header = true},
          actionEntry(Action::SingleClick, request, true),
          actionEntry(Action::DoubleClick, request, true),
          {.label = "Typing in the file view", .header = true},
          actionEntry(Action::TypeToSearch, request, true),
          actionEntry(Action::Typeahead, request, true)
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
      std::vector<ContextMenuControlEntry> entries{
          {.id = 9000, .label = "New Tab", .shortcut = "Ctrl+T"},
          {.id = 9001, .label = "New Window", .shortcut = "Ctrl+N"},
          {.id = 9002, .label = "Split View", .shortcut = "F3"},
          {.label = "", .separator = true}
      };
      const auto request = actionRequest();
      for (const auto action : {Action::Grid, Action::List, Action::Miller}) {
        entries.push_back(actionEntry(action, request, true));
      }
      entries.push_back({.label = "", .separator = true});
      for (auto action :
           {Action::SortMenu, Action::SelectionMenu, Action::InteractionMenu, Action::ShowHidden, Action::ShowSidebar,
            Action::ShowPreview, Action::Search, Action::Settings, Action::About}) {
        entries.push_back(actionEntry(action, request));
      }
      showContextMenu(
          std::move(entries),
          [this, request](int id) {
            if (id >= 9000) {
              if (m_workspaceRequest) {
                m_workspaceRequest(
                    id == 9000       ? WorkspaceCommand::NewTab
                        : id == 9001 ? WorkspaceCommand::NewWindow
                                     : WorkspaceCommand::Split,
                    std::nullopt
                );
              }
            } else {
              dispatchAction(static_cast<Action>(id), request);
            }
          },
          sceneRect(*m_menuButton), true
      );
    }

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
    files::PinService& m_pins;
    files::FileLauncher m_launcher;
    files::PlacesSidebar* m_places = nullptr;
    bool m_placesDirty = true;
    std::string m_actionError;
    std::optional<files::Location> m_revealTarget;
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
    )
        : m_connection(connection), m_renderer(renderer), m_input(input), m_clipboard(clipboard), m_defaults(settings),
          m_preferences(files::ConfigPaths::fromEnvironment()), m_pins(files::StateStore::pathFromEnvironment()) {
      m_preferences.setChangedCallback([this](const std::string& error) {
        if (m_preferences.settings()) {
          m_defaults = m_preferences.settings()->browser;
        }
        for (auto& [id, page] : m_pages) {
          (void)id;
          page->preferencesChanged(error);
        }
      });
      m_clipboard.setChangedCallback([this] {
        for (auto& [id, page] : m_pages) {
          (void)id;
          page->clipboardChanged();
        }
      });
      m_clipboard.setDropCallbacks(
          [this](wl_surface* surface, float x, float y) {
            clearExternalDrops();
            const std::vector<files::Bookmark>* pins = nullptr;
            for (const auto& [id, page] : m_pages) {
              (void)id;
              if (const auto* folders = page->draggedFolders()) {
                pins = folders;
              }
            }
            for (const auto& [windowId, host] : m_hosts) {
              if (host->closed || host->surface.wlSurface() != surface) {
                continue;
              }
              if (host->tooltip) {
                host->tooltip->dismiss();
              }
              const auto found = m_model.windows().find(windowId);
              if (found == m_model.windows().end()) {
                return files::FileDropTarget{};
              }
              for (const auto& [tab, button] : host->buttons) {
                if (host->tabScroll->containsScenePoint(x, y) && button->containsScenePoint(x, y)) {
                  if (const auto layout = m_model.layout(tab)) {
                    if (const auto destination = m_pages.at(layout->active)->location()) {
                      float bx = 0, by = 0;
                      Node::absolutePosition(button, bx, by);
                      return m_pages.at(found->second.active)
                          ->externalDestination(*destination, LayoutRect{bx, by, button->width(), button->height()});
                    }
                  }
                }
              }
              for (const auto pageId : host->mounted) {
                if (m_pages.at(pageId)->contains(x, y)) {
                  return m_pages.at(pageId)->externalDrop(x, y, pins);
                }
              }
              if (found->second.active) {
                return m_pages.at(found->second.active)->externalDrop(x, y, pins);
              }
            }
            return files::FileDropTarget{};
          },
          [this] { clearExternalDrops(); }
      );
      m_preferences.startWatching();
      m_pins.setChangedCallback([this] {
        for (auto& [id, page] : m_pages) {
          (void)id;
          page->placesChanged();
        }
      });
      m_icons.setChangedCallback([this] {
        for (auto& [id, page] : m_pages) {
          (void)id;
          page->iconsReady();
        }
      });
      m_pins.reload();
    }
    SharedTextureCache& imageTextures() { return m_icons.textures(); }

    ~WorkspaceApplication() {
      m_lifetime.reset();
      m_clipboard.setChangedCallback({});
      m_clipboard.setDropCallbacks({}, {});
      m_preferences.setChangedCallback({});
      m_pins.setChangedCallback({});
      m_icons.setChangedCallback({});
      for (auto& [id, host] : m_hosts) {
        (void)id;
        releaseChrome(*host);
        for (auto tab : host->mounted) {
          m_pages.at(tab)->unmount();
        }
        host->mounted.clear();
        host->surface.setSceneRoot(nullptr);
        host->root.reset();
      }
      m_pages.clear();
    }
    void start(const std::optional<std::string>& initial) {
      const auto id = newWindow();
      if (!id) {
        throw std::runtime_error("Cannot create a Files window.");
      }
      newTab(
          id,
          initial
              ? std::optional(
                    files::Location::fromUri(files::resolveLocationText(*initial, files::Location::fromPath("/")).uri())
                )
              : std::nullopt
      );
    }
    void focus(wl_surface* surface, bool entered) {
      for (const auto& [id, host] : m_hosts) {
        if (host->surface.wlSurface() == surface) {
          if (!entered) {
            if (host->tooltip) {
              host->tooltip->dismiss();
            }
            host->surface.requestUpdate();
            return;
          }
          m_focused = id;
          if (auto found = m_model.windows().find(id); found != m_model.windows().end() && found->second.active) {
            m_pages.at(found->second.active)->nativeFocus();
          }
        }
      }
    }
    void outputChanged(wl_output* output) {
      for (auto& [id, host] : m_hosts) {
        (void)id;
        if (m_connection.outputForSurface(host->surface.wlSurface()) == output) {
          host->surface.onSurfaceOutputEnter(host->surface.wlSurface(), output);
        }
      }
      for (auto& [id, page] : m_pages) {
        (void)id;
        page->outputChanged(output);
      }
    }
    void iconsChanged() {
      m_icons.invalidate();
      for (auto& [id, page] : m_pages) {
        (void)id;
        page->iconThemeChanged();
      }
    }
    bool running() {
      bool live = false;
      for (auto& [id, host] : m_hosts) {
        if (g_stopRequested && host->surface.isRunning()) {
          host->surface.close();
        }
        if (!host->surface.isRunning() && !host->closed) {
          host->closed = true;
          if (host->tooltip) {
            host->tooltip->dismiss();
          }
          releaseChrome(*host);
          for (auto tab : host->mounted) {
            m_pages.at(tab)->hide();
            m_pages.at(tab)->unmount();
          }
          host->mounted.clear();
          if (auto found = m_model.windows().find(id); found != m_model.windows().end()) {
            const auto tabs = found->second.tabs;
            for (auto tab : tabs) {
              m_model.close(tab);
            }
            m_model.removeWindow(id);
          }
        }
        live |= host->surface.isRunning();
      }
      bool busy = false;
      for (auto& [id, page] : m_pages) {
        if (!live) {
          page->shutdown();
        } else if (!m_model.owner(id)) {
          for (auto& [window, host] : m_hosts) {
            if (host->surface.isRunning()) {
              (void)window;
              if (page->boundSurface() != &host->surface) {
                page->rebind(host->surface);
              }
              break;
            }
          }
          page->pumpJobs();
        }
        bool visible = false;
        for (const auto& [window, host] : m_hosts) {
          (void)window;
          visible |= std::ranges::contains(host->mounted, id);
        }
        if (live && !visible) {
          page->pumpJobs();
        }
        busy |= page->busy();
      }
      if (m_cacheClearOwner && !m_pages.at(*m_cacheClearOwner)->clearingThumbnails()) {
        m_cacheClearOwner.reset();
        for (auto& [id, page] : m_pages) {
          (void)id;
          page->pauseThumbnails(false);
        }
      }
      std::vector<files::TabId> retired;
      for (auto& [id, page] : m_pages) {
        const auto tab = m_model.tabFor(id);
        const bool referenced = tab
            && std::ranges::contains(m_model.pages(*tab), id)
            && (m_model.owner(id) || std::ranges::contains(m_model.closed(), *tab));
        const bool mounted = std::ranges::any_of(m_hosts, [id](const auto& item) {
          return std::ranges::contains(item.second->mounted, id);
        });
        if (!referenced && !mounted && !page->busy()) {
          page->shutdown();
          if (!page->busy()) {
            retired.push_back(id);
          } else {
            busy = true;
          }
        }
      }
      for (auto id : retired) {
        m_pages.erase(id);
      }
      if (live) {
        std::vector<files::WindowId> retiredHosts;
        for (const auto& [id, host] : m_hosts) {
          if (host->closed && std::ranges::none_of(m_pages, [&](const auto& item) {
                return item.second->boundSurface() == &host->surface;
              })) {
            retiredHosts.push_back(id);
          }
        }
        for (auto id : retiredHosts) {
          m_hosts.erase(id);
        }
      }
      return live || busy;
    }
    void keyboard(const KeyboardEvent& event) {
      if (event.pressed) {
        for (const auto& [id, host] : m_hosts) {
          (void)id;
          if (host->tooltip) {
            host->tooltip->dismiss();
          }
          host->surface.requestUpdate();
        }
      }
      for (auto& [id, page] : m_pages) {
        if (page->popupOwns(m_connection.lastKeyboardSurface())) {
          (void)id;
          page->keyboard(event);
          return;
        }
      }
      auto hostIt = m_hosts.find(m_focused);
      if (hostIt == m_hosts.end() || hostIt->second->closed) {
        return;
      }
      auto& host = *hostIt->second;
      const auto record = m_model.windows().find(host.id);
      if (record == m_model.windows().end() || !record->second.active) {
        return;
      }
      auto& page = *m_pages.at(record->second.active);
      if (page.popup()) {
        page.keyboard(event);
        return;
      }
      if (event.pressed && !event.preedit) {
        if (event.modifiers == KeyMod::Ctrl && event.sym == XKB_KEY_q) {
          g_stopRequested = 1;
          host.surface.requestUpdate();
          return;
        }
        if (event.modifiers == (KeyMod::Ctrl | KeyMod::Shift) && (event.sym == XKB_KEY_w || event.sym == XKB_KEY_W)) {
          host.surface.close();
          return;
        }
        const bool ctrl = event.modifiers == KeyMod::Ctrl,
                   ctrlShift = event.modifiers == (KeyMod::Ctrl | KeyMod::Shift);
        std::optional<WorkspaceCommand> command;
        if (event.modifiers == (KeyMod::Ctrl | KeyMod::Alt) && (event.sym == XKB_KEY_w || event.sym == XKB_KEY_W)) {
          command = WorkspaceCommand::ClosePane;
        }
        if (ctrl && event.sym == XKB_KEY_t) {
          command = WorkspaceCommand::NewTab;
        }
        if (ctrlShift && (event.sym == XKB_KEY_t || event.sym == XKB_KEY_T)) {
          command = WorkspaceCommand::Reopen;
        }
        if (ctrl && event.sym == XKB_KEY_w) {
          command = WorkspaceCommand::Close;
        }
        if (ctrl && event.sym == XKB_KEY_n) {
          command = WorkspaceCommand::NewWindow;
        }
        if (event.modifiers == 0 && event.sym == XKB_KEY_F3) {
          command = WorkspaceCommand::Split;
        }
        if (event.modifiers == 0 && event.sym == XKB_KEY_F6) {
          command = WorkspaceCommand::SwitchArea;
        }
        if (ctrlShift && (event.sym == XKB_KEY_d || event.sym == XKB_KEY_D)) {
          command = WorkspaceCommand::Duplicate;
        }
        if (command) {
          defer(host.id, record->second.active, *command);
          return;
        }
        if (!page.overlay()
            && KeySymbol::isTab(event.sym)
            && (event.modifiers == 0 || event.modifiers == KeyMod::Shift)) {
          if (page.completeLocationOnTab(event.modifiers)) {
            return;
          }
          auto* target = page.cycleFocus(event.modifiers == KeyMod::Shift);
          for (auto tab : host.mounted) {
            if (tab != record->second.active && m_pages.at(tab)->owns(target)) {
              page.focusArea(nullptr);
              m_model.focus(host.id, tab);
              host.surface.requestLayout();
              prepare(host);
              m_pages.at(tab)->focusArea(target);
              break;
            }
          }
          host.surface.requestRedraw();
          return;
        }

        if ((ctrl || ctrlShift)
            && (KeySymbol::isTab(event.sym) || event.sym == XKB_KEY_Page_Down || event.sym == XKB_KEY_Page_Up)) {
          const auto& tabs = record->second.tabs;
          const auto found = std::ranges::find(tabs, record->second.selected);
          const auto index = static_cast<std::size_t>(found - tabs.begin());
          const bool backwards = ctrlShift || event.sym == XKB_KEY_Page_Up;
          select(host.id, tabs[(index + tabs.size() + (backwards ? -1 : 1)) % tabs.size()]);
          return;
        }
        if (event.modifiers == KeyMod::Alt && event.sym >= XKB_KEY_1 && event.sym <= XKB_KEY_9) {
          const auto index =
              event.sym == XKB_KEY_9 ? record->second.tabs.size() - 1 : static_cast<std::size_t>(event.sym - XKB_KEY_1);
          if (index < record->second.tabs.size()) {
            select(host.id, record->second.tabs[index]);
          }
          return;
        }
        if (event.sym == XKB_KEY_Escape && host.pressedTab) {
          host.pressedTab.reset();
          host.tabDrag = false;
          host.surface.requestLayout();
          return;
        }
        if (event.sym == XKB_KEY_Escape && host.resizing) {
          m_model.resize(host.id, host.resizeStart);
          host.resizing = false;
          host.surface.requestLayout();
          return;
        }
        if (event.sym == XKB_KEY_Escape && host.previewDragWidth) {
          host.previewDragWidth.reset();
          host.surface.requestLayout();
          return;
        }
      }
      page.keyboard(event);
    }
    void pointer(const PointerEvent& event) {
      for (auto& [id, page] : m_pages) {
        if (page->popupOwns(event.surface)) {
          (void)id;
          page->pointer(event);
          return;
        }
      }
      Host* host = nullptr;
      for (auto& [id, value] : m_hosts) {
        (void)id;
        if (value->surface.wlSurface() == event.surface) {
          host = value.get();
          break;
        }
      }
      if (!host || host->closed) {
        return;
      }
      if (host->tooltip) {
        if (!event.touch && (event.type == PointerEvent::Type::Enter || event.type == PointerEvent::Type::Motion)) {
          host->tooltip->resume();
        } else {
          host->tooltip->dismiss();
        }
        host->surface.requestUpdate();
      }
      host->pointerX = static_cast<float>(event.sx);
      host->pointerY = static_cast<float>(event.sy);
      if ((event.type == PointerEvent::Type::Button && event.pressed)
          || (event.type != PointerEvent::Type::Leave && m_connection.lastKeyboardSurface() == event.surface)) {
        m_focused = host->id;
      }
      const auto found = m_model.windows().find(host->id);
      if (found == m_model.windows().end() || !found->second.active) {
        return;
      }
      const auto& window = found->second;
      auto& active = *m_pages.at(window.active);
      if (host->dispatcher.pointerCaptured()) {
        dispatch(host->dispatcher, event);
        return;
      }
      if (event.type == PointerEvent::Type::Leave
          || !host->strip
          || !host->strip->containsScenePoint(static_cast<float>(event.sx), static_cast<float>(event.sy))) {
        host->dispatcher.pointerLeave();
      }
      for (auto& [tab, page] : m_pages) {
        (void)tab;
        if (page->popup() && page->boundSurface() == &host->surface) {
          page->pointer(event);
          return;
        }
      }
      for (auto tab : host->mounted) {
        if (m_pages.at(tab)->dragging() || m_pages.at(tab)->pointerCaptured()) {
          m_pages.at(tab)->pointer(event);
          return;
        }
      }
      const auto x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
      if (host->previewDragWidth) {
        if (event.type == PointerEvent::Type::Motion) {
          host->previewDragWidth =
              std::clamp(static_cast<int>(std::lround(host->surface.width() - kWorkspaceInset - x)), 208, 800);
          host->surface.requestLayout();
        } else if (event.type == PointerEvent::Type::Button && !event.pressed && event.button == BTN_LEFT) {
          active.savePreviewWidth(*host->previewDragWidth);
          host->previewDragWidth.reset();
        } else if (event.type == PointerEvent::Type::Leave) {
          host->previewDragWidth.reset();
          host->surface.requestLayout();
        }
        return;
      }
      if (host->resizing) {
        if (event.type == PointerEvent::Type::Motion) {
          m_model.resize(host->id, (x - host->browsingBounds.x) / host->browsingBounds.width);
          host->surface.requestLayout();
        }
        if (event.type == PointerEvent::Type::Button && !event.pressed) {
          host->resizing = false;
        }
        return;
      }
      if (host->pressedTab) {
        if (event.type == PointerEvent::Type::Motion) {
          host->tabDrag |= std::abs(x - host->pressX) > 8 || std::abs(y - host->pressY) > 8;
          return;
        }
        if (event.type == PointerEvent::Type::Button && !event.pressed && event.button == BTN_LEFT) {
          const auto tab = *std::exchange(host->pressedTab, std::nullopt);
          if (host->tabDrag) {
            if (y > 90) {
              defer(host->id, tab, WorkspaceCommand::Detach);
            } else {
              std::size_t index = 0;
              for (const auto& [id, button] : host->buttons) {
                (void)id;
                float bx = 0, by = 0;
                Node::absolutePosition(button, bx, by);
                if (x > bx + button->width() * 0.5F) {
                  ++index;
                }
              }
              m_model.move(tab, host->id, index);
              host->labels.clear();
              host->surface.requestLayout();
            }
          } else {
            select(host->id, tab);
          }
          host->tabDrag = false;
          return;
        }
      }
      if (host->strip && host->strip->containsScenePoint(x, y)) {
        for (auto tab : host->mounted) {
          m_pages.at(tab)->leavePointer();
        }
        for (const auto& [tab, button] : host->buttons) {
          if (host->tabScroll->containsScenePoint(x, y)
              && button->containsScenePoint(x, y)
              && !host->closeButtons.at(tab).first->containsScenePoint(x, y)
              && event.type == PointerEvent::Type::Button
              && event.pressed) {
            if (event.button == BTN_MIDDLE) {
              defer(host->id, tab, WorkspaceCommand::Close);
              return;
            }
            if (event.button == BTN_RIGHT) {
              tabMenu(*host, tab, x, y);
              return;
            }
            if (event.button == BTN_LEFT) {
              host->pressedTab = tab;
              host->pressX = x;
              host->pressY = y;
              return;
            }
          }
        }
        if (event.type == PointerEvent::Type::Button
            && event.pressed
            && event.button == BTN_LEFT
            && host->tabScroll->containsScenePoint(x, y)) {
          bool child = host->newTabButton->containsScenePoint(x, y);
          for (const auto& [tab, button] : host->buttons) {
            (void)tab;
            float bx = 0, by = 0;
            Node::absolutePosition(button, bx, by);
            child |= x >= bx && x <= bx + button->width();
          }
          if (!child) {
            host->surface.beginMove(event.serial);
            return;
          }
        }
        dispatch(host->dispatcher, event);
        return;
      }
      if (host->previewDivider
          && host->previewDivider->visible()
          && host->previewDivider->containsScenePoint(x, y)
          && event.type == PointerEvent::Type::Button
          && event.pressed
          && event.button == BTN_LEFT) {
        host->previewDragWidth = active.previewWidth();
        return;
      }
      if (host->divider
          && host->divider->visible()
          && host->divider->containsScenePoint(x, y)
          && event.type == PointerEvent::Type::Button
          && event.pressed
          && event.button == BTN_LEFT) {
        host->resizing = true;
        host->resizeStart = window.fraction;
        return;
      }
      if (!active.overlay() && event.type == PointerEvent::Type::Button && event.pressed) {
        for (auto tab : host->mounted) {
          if (m_pages.at(tab)->contains(x, y)) {
            if (tab != window.active) {
              m_model.focus(host->id, tab);
              host->surface.requestLayout();
              prepare(*host);
            }
            break;
          }
        }
      }
      m_pages.at(m_model.windows().at(host->id).active)->pointer(event);
    }

  private:
    void clearExternalDrops() {
      for (auto& [id, page] : m_pages) {
        (void)id;
        page->clearExternalDrop();
      }
    }
    static void dispatch(InputDispatcher& dispatcher, const PointerEvent& event) {
      const auto x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
      switch (event.type) {
      case PointerEvent::Type::Enter:
        dispatcher.pointerEnter(x, y, event.serial);
        break;
      case PointerEvent::Type::Leave:
        dispatcher.pointerLeave();
        break;
      case PointerEvent::Type::Motion:
        dispatcher.pointerMotion(x, y, event.serial);
        break;
      case PointerEvent::Type::Button:
        dispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
        break;
      case PointerEvent::Type::Axis:
        dispatcher.pointerAxis(
            x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120,
            event.axisLines, event.axisGestureSerial
        );
        break;
      }
    }
    files::WindowId newWindow() {
      const auto id = m_model.addWindow();
      if (!id) {
        return 0;
      }
      auto host = std::make_unique<Host>(id, m_connection);
      auto* raw = host.get();
      host->surface.setRenderContext(&m_renderer);
      host->surface.setAnimationManager(&m_animations);
      host->surface.setPrepareFrameCallback([this, raw](bool, bool) { prepare(*raw); });
      host->surface.setUpdateCallback([] {});
      host->surface.setConfigureCallback([this, raw](std::uint32_t, std::uint32_t) {
        if (raw->tooltip) {
          raw->tooltip->dismiss();
        }
        raw->previewDragWidth.reset();
        for (auto& [tab, page] : m_pages) {
          (void)tab;
          if (page->boundSurface() == &raw->surface) {
            page->configurationChanged();
          }
        }
        raw->surface.requestLayout();
      });
      host->dispatcher.setCursorShapeCallback([this](auto serial, auto shape) {
        m_connection.extension<files::DesktopIntegration>().setCursorShape(serial, shape);
      });
      if (!host->surface.initialize(
              nullptr,
              {.width = 1080,
               .height = 720,
               .minWidth = 620,
               .minHeight = 400,
               .title = "Noctalia Files",
               .appId = "dev.noctalia.Files"}
          )) {
        throw std::runtime_error("Cannot initialize a Files window.");
      }
      m_hosts.emplace(id, std::move(host));
      m_focused = id;
      return id;
    }
    files::TabId newTab(files::WindowId window, std::optional<files::Location> location, files::TabId duplicate = 0) {
      if (m_pages.size() >= 64) {
        return 0;
      }
      const auto previous = m_model.windows().at(window).active;
      const auto id = m_model.addTab(window);
      if (!id) {
        return 0;
      }
      return createPage(window, id, std::move(location), duplicate, previous);
    }
    files::TabId newPane(files::WindowId window, std::optional<files::Location> location, files::TabId duplicate = 0) {
      if (m_pages.size() >= 64) {
        return 0;
      }
      const auto previous = m_model.windows().at(window).active;
      const auto id = m_model.addPane(window);
      if (!id) {
        return 0;
      }
      return createPage(window, id, std::move(location), duplicate, previous);
    }
    files::TabId createPage(
        files::WindowId window, files::TabId id, std::optional<files::Location> location, files::TabId duplicate,
        files::TabId previous
    ) {
      auto& host = *m_hosts.at(window);
      const auto settings = duplicate              ? m_pages.at(duplicate)->options()
          : previous && m_pages.contains(previous) ? m_pages.at(previous)->options()
                                                   : m_defaults;
      auto page = std::make_unique<BrowserPage>(
          m_connection, m_renderer, m_input, settings, m_clipboard, host.surface, m_animations, m_pins, m_icons,
          m_preferences
      );
      page->setWorkspaceId(id);
      page->setClearThumbnails([this, id] {
        if (m_cacheClearOwner) {
          return;
        }
        for (auto& [pageId, page] : m_pages) {
          (void)pageId;
          page->pauseThumbnails(true);
        }
        m_cacheClearOwner = id;
        m_pages.at(id)->clearThumbnails();
      });
      page->setHasOther([this, id] {
        const auto owner = m_model.owner(id);
        return owner && m_model.other(*owner).has_value();
      });
      page->setWorkspaceRequest([this, id](WorkspaceCommand command, std::optional<files::Location> destination) {
        if (auto owner = m_model.owner(id)) {
          defer(*owner, id, command, std::move(destination));
        }
      });
      page->setPopupAvailable([this, id] {
        const auto page = m_pages.find(id);
        if (page == m_pages.end()) {
          return false;
        }
        for (const auto& [other, value] : m_pages) {
          if (other != id && value->boundSurface() == page->second->boundSurface() && value->popup()) {
            return false;
          }
        }
        return true;
      });
      page->setForeignDrop([this, id](float x, float y) -> std::optional<std::pair<files::Location, LayoutRect>> {
        const auto owner = m_model.owner(id);
        if (!owner) {
          return std::nullopt;
        }
        auto& host = *m_hosts.at(*owner);
        for (auto other : host.mounted) {
          if (other != id) {
            if (auto target = m_pages.at(other)->foreignTarget(x, y)) {
              return target;
            }
          }
        }
        for (const auto& [tab, button] : host.buttons) {
          if (tab != id && host.tabScroll->containsScenePoint(x, y) && button->containsScenePoint(x, y)) {
            if (auto destination = m_pages.at(m_model.layout(tab)->active)->location()) {
              float bx = 0, by = 0;
              Node::absolutePosition(button, bx, by);
              return std::pair(*destination, LayoutRect{bx, by, button->width(), button->height()});
            }
          }
        }
        return std::nullopt;
      });
      page->initialize(location ? std::optional(location->uri()) : std::nullopt);
      if (duplicate) {
        page->duplicateFrom(*m_pages.at(duplicate));
      }
      m_pages.emplace(id, std::move(page));
      if (m_cacheClearOwner) {
        m_pages.at(id)->pauseThumbnails(true);
      }
      host.labels.clear();
      host.surface.requestLayout();
      return id;
    }
    void select(files::WindowId window, files::TabId tab) {
      if (m_model.select(window, tab)) {
        m_hosts.at(window)->surface.requestLayout();
        m_focused = window;
      }
    }
    void defer(
        files::WindowId window, files::TabId tab, WorkspaceCommand command,
        std::optional<files::Location> location = std::nullopt
    ) {
      if ((command == WorkspaceCommand::CopyOther || command == WorkspaceCommand::MoveOther) && !location) {
        if (auto other = m_model.other(window)) {
          location = m_pages.at(*other)->location();
        }
      }
      const std::weak_ptr<int> lifetime = m_lifetime;
      DeferredCall::callLater([this, lifetime, window, tab, command, location = std::move(location)] {
        if (!lifetime.expired() && m_model.owner(tab) == window) {
          execute(window, tab, command, location);
        }
      });
    }
    void execute(
        files::WindowId window, files::TabId tab, WorkspaceCommand command,
        const std::optional<files::Location>& destination
    ) {
      auto& host = *m_hosts.at(window);
      const auto logical = m_model.tabFor(tab);
      const auto layout = m_model.layout(tab);
      const auto source = (logical && *logical == tab && layout) ? layout->active : tab;
      auto& page = *m_pages.at(source);
      if (logical
          && (command == WorkspaceCommand::Duplicate
              || command == WorkspaceCommand::Split
              || command == WorkspaceCommand::Swap)) {
        m_model.select(window, *logical);
      }
      switch (command) {
      case WorkspaceCommand::ClosePane:
        m_model.closePane(window, source);
        break;
      case WorkspaceCommand::NewTab:
        if (!newTab(window, destination ? destination : page.location())) {
          page.workspaceError("Close a tab before opening another.");
        }
        break;
      case WorkspaceCommand::Duplicate: {
        const auto record = m_model.windows().at(window);
        const auto cloned = newTab(window, m_pages.at(record.primary)->location(), record.primary);
        if (cloned && record.secondary) {
          const auto auxiliary = newPane(window, m_pages.at(*record.secondary)->location(), *record.secondary);
          m_model.resize(window, record.fraction);
          if (record.active == record.primary) {
            m_model.focus(window, cloned);
          } else if (auxiliary) {
            m_model.focus(window, auxiliary);
          }
        }
        break;
      }
      case WorkspaceCommand::Close:
        m_model.close(tab);
        break;
      case WorkspaceCommand::Reopen:
        if (m_model.reopen(window)) {
          for (auto& [pageId, value] : m_pages) {
            if (m_model.tabFor(pageId) == m_model.windows().at(window).selected) {
              value->rebind(host.surface);
            }
          }
        }
        break;
      case WorkspaceCommand::NewWindow: {
        if (auto id = newWindow()) {
          if (!newTab(id, destination ? destination : page.location())) {
            m_hosts.at(id)->surface.close();
            page.workspaceError("Close a tab before opening another window.");
          }
        } else {
          page.workspaceError("The eight-window limit has been reached.");
        }
        break;
      }
      case WorkspaceCommand::Detach:
      case WorkspaceCommand::MoveWindow: {
        files::WindowId destinationWindow = 0;
        if (command == WorkspaceCommand::Detach) {
          destinationWindow = newWindow();
        } else {
          for (const auto& [id, target] : m_hosts) {
            if (id > window && !target->closed) {
              destinationWindow = id;
              break;
            }
          }
          if (!destinationWindow) {
            for (const auto& [id, target] : m_hosts) {
              if (id != window && !target->closed) {
                destinationWindow = id;
                break;
              }
            }
          }
        }
        if (destinationWindow) {
          std::vector<files::TabId> pages;
          for (const auto& [pageId, value] : m_pages) {
            (void)value;
            if (m_model.tabFor(pageId) == logical) {
              pages.push_back(pageId);
            }
          }
          releaseChrome(host);
          for (auto pageId : pages) {
            m_pages.at(pageId)->hide();
            m_pages.at(pageId)->unmount();
            std::erase(host.mounted, pageId);
          }
          m_model.move(tab, destinationWindow, m_model.windows().at(destinationWindow).tabs.size());
          for (auto pageId : pages) {
            m_pages.at(pageId)->rebind(m_hosts.at(destinationWindow)->surface);
          }
          m_hosts.at(destinationWindow)->surface.requestLayout();
        }
        break;
      }
      case WorkspaceCommand::Split:
        if (m_model.windows().at(window).secondary) {
          m_model.unsplit(window);
        } else if (!newPane(window, page.location(), source)) {
          page.workspaceError("The browser-page limit has been reached.");
        }
        break;
      case WorkspaceCommand::SwitchArea:
        if (auto other = m_model.other(window)) {
          m_model.focus(window, *other);
        }
        break;
      case WorkspaceCommand::Swap:
        m_model.swap(window);
        break;
      case WorkspaceCommand::OpenOther:
        if (auto other = m_model.other(window)) {
          if (destination) {
            m_pages.at(*other)->open(*destination);
          }
        } else {
          newPane(window, destination ? destination : page.location());
        }
        break;
      case WorkspaceCommand::CopyOther:
      case WorkspaceCommand::MoveOther:
        if (destination) {
          page.transferTo(*destination, command == WorkspaceCommand::CopyOther);
        }
        break;
      }
      host.labels.clear();
      host.surface.requestLayout();
      if (m_model.windows().at(window).tabs.empty()) {
        host.surface.close();
      }
    }
    void releaseChrome(Host& host) {
      if (!host.chromeOwner) {
        return;
      }
      auto& page = *m_pages.at(host.chromeOwner);
      page.deactivate();
      if (host.toolbarNode && host.toolbarNode->parent()) {
        page.returnToolbar(host.toolbarNode->parent()->removeChild(host.toolbarNode));
      }
      host.toolbarNode = nullptr;
      page.returnSidebar();
      host.chromeOwner = 0;
    }
    void tabMenu(Host& host, files::TabId tab, float x, float y) {
      auto& page = *m_pages.at(m_model.windows().at(host.id).active);
      std::vector<ContextMenuControlEntry> entries{
          {.id = 0, .label = "New Tab", .shortcut = "Ctrl+T"},
          {.id = 1, .label = "Duplicate Tab", .shortcut = "Ctrl+Shift+D"},
          {.id = 2, .label = "Close Tab", .shortcut = "Ctrl+W"},
          {.id = 3, .label = "Reopen Closed Tab", .shortcut = "Ctrl+Shift+T"},
          {.label = "", .separator = true},
          {.id = 4, .label = "Split View", .shortcut = "F3"},
          {.id = 5, .label = "Swap Panes"},
          {.id = 6, .label = "Move Tab to New Window"},
          {.id = 7, .label = "Move Tab to Next Window"}
      };
      const auto window = host.id;
      page.workspaceMenu(
          std::move(entries),
          [this, window, tab](int id) {
            const std::array commands{WorkspaceCommand::NewTab, WorkspaceCommand::Duplicate, WorkspaceCommand::Close,
                                      WorkspaceCommand::Reopen, WorkspaceCommand::Split,     WorkspaceCommand::Swap,
                                      WorkspaceCommand::Detach, WorkspaceCommand::MoveWindow};
            if (id >= 0 && static_cast<std::size_t>(id) < commands.size()) {
              defer(window, tab, commands[static_cast<std::size_t>(id)]);
            }
          },
          x, y
      );
    }
    static Button::ButtonPalette closeTabPalette(ColorRole foreground) {
      auto palette = Button::defaultPalette(ButtonVariant::Ghost);
      palette.normal.label = colorSpecFromRole(foreground);
      palette.hover.bg = colorSpecFromRole(foreground, 0.12F);
      palette.hover.label = colorSpecFromRole(foreground);
      palette.pressed.bg = colorSpecFromRole(foreground, 0.20F);
      palette.pressed.label = colorSpecFromRole(foreground);
      return palette;
    }
    void rebuildStrip(Host& host, const files::TabWindow& window) {
      std::vector<std::pair<files::TabId, std::string>> labels;
      labels.reserve(window.tabs.size());
      for (auto id : window.tabs) {
        labels.emplace_back(id, m_pages.at(m_model.layout(id)->active)->title());
      }
      const bool decorated = !host.surface.serverDecorated() && !host.surface.fullscreen();
      if (host.strip && labels == host.labels && decorated == host.decorated) {
        return;
      }
      host.decorated = decorated;
      const float scrollOffset = host.tabScroll ? host.tabScroll->scrollOffset() : 0.0F;
      host.dispatcher.cancelPointerCapture();
      if (host.strip) {
        auto removed = host.root->removeChild(host.strip);
      }
      host.buttons.clear();
      host.closeButtons.clear();
      host.revealedTab = 0;
      host.labels = std::move(labels);
      auto scroll = ui::scrollView(
          {.out = &host.tabScroll,
           .orientation = ScrollOrientation::Horizontal,
           .scrollbarVisible = false,
           .viewportPaddingH = 0.0F,
           .viewportPaddingV = 0.0F,
           .fillWidth = true,
           .fillHeight = true,
           .flexGrow = 1}
      );
      auto tabs = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs, .height = 40.0F});
      scroll->content()->setDirection(FlexDirection::Horizontal);
      const auto windowId = host.id;
      auto tabPalette = Button::defaultPalette(ButtonVariant::Ghost);
      tabPalette.hover.bg = colorSpecFromRole(ColorRole::OnSurface, 0.08F);
      tabPalette.hover.label = colorSpecFromRole(ColorRole::OnSurface);
      for (const auto& [id, label] : host.labels) {
        Button* button = nullptr;
        auto tab = ui::button(
            {.out = &button,
             .text = label,
             .controlHeight = 38.0F,
             .contentAlign = ButtonContentAlign::Start,
             .variant = ButtonVariant::Ghost,
             .customPalette = tabPalette,
             .minWidth = 112.0F,
             .maxWidth = 220.0F,
             .paddingV = 0.0F,
             .paddingH = Style::spaceXs,
             .gap = Style::spaceXs,
             .onClick = [this, windowId, id] { select(windowId, id); },
             .configure =
                 [](Button& button) {
                   button.label()->setUseMarkup(false);
                   button.label()->setFlexGrow(1);
                   button.label()->setTextAlign(TextAlign::Center);
                 }}
        );
        host.buttons.emplace_back(id, button);
        Button* close = nullptr;
        const auto foreground = id == window.selected ? ColorRole::OnPrimary : ColorRole::OnSurface;
        tab->addChild(
            ui::button(
                {.out = &close,
                 .glyph = "x",
                 .controlHeight = Style::controlHeightSm,
                 .variant = ButtonVariant::Ghost,
                 .customPalette = closeTabPalette(foreground),
                 .tooltip = "Close tab",
                 .onClick = [this, windowId, id] { defer(windowId, id, WorkspaceCommand::Close); },
                 .configure = [](Button& button) { button.setZIndex(2); }}
            )
        );
        host.closeButtons.emplace(id, std::pair(close, foreground));
        tabs->addChild(std::move(tab));
      }
      tabs->addChild(
          ui::button(
              {.out = &host.newTabButton,
               .glyph = "plus",
               .variant = ButtonVariant::Ghost,
               .tooltip = "New tab (Ctrl+T)",
               .onClick = [this, windowId] {
                 defer(windowId, m_model.windows().at(windowId).active, WorkspaceCommand::NewTab);
               }}
          )
      );
      scroll->content()->addChild(std::move(tabs));
      scroll->requestScrollToOffset(scrollOffset);
      auto strip = ui::row(
          {.out = &host.strip,
           .align = FlexAlign::Center,
           .gap = Style::spaceXs,
           .paddingH = Style::spaceSm,
           .fill = colorSpecFromRole(ColorRole::SurfaceContainer),
           .height = 40.0F,
           .participatesInLayout = false},
          std::move(scroll),
          ui::button(
              {.out = &host.splitButton,
               .glyph = "layout-columns",
               .selected = window.secondary.has_value(),
               .variant = ButtonVariant::Ghost,
               .tooltip = "Split view (F3)",
               .onClick = [this, windowId] {
                 defer(windowId, m_model.windows().at(windowId).active, WorkspaceCommand::Split);
               }}
          )
      );
      if (!host.surface.serverDecorated() && !host.surface.fullscreen()) {
        strip->addChild(
            ui::button(
                {.glyph = "minus", .variant = ButtonVariant::Ghost, .tooltip = "Minimize window", .onClick = [&host] {
                   host.surface.minimize();
                 }}
            )
        );
        strip->addChild(
            ui::button(
                {.glyph = "square", .variant = ButtonVariant::Ghost, .tooltip = "Toggle maximized", .onClick = [&host] {
                   host.surface.toggleMaximized();
                 }}
            )
        );
        strip->addChild(
            ui::button(
                {.glyph = "x",
                 .variant = ButtonVariant::Ghost,
                 .tooltip = "Close window (Ctrl+Shift+W)",
                 .onClick = [&host] { host.surface.close(); }}
            )
        );
      }
      strip->setZIndex(50);
      host.root->addChild(std::move(strip));
    }
    void syncTooltip(Host& host) {
      if (!host.tooltip) {
        return;
      }
      if (host.resizing
          || host.previewDragWidth
          || host.pressedTab
          || host.dispatcher.pointerCaptured()
          || std::ranges::any_of(
              m_pages,
              [&host](const auto& item) { return item.second->boundSurface() == &host.surface && item.second->popup(); }
          )
          || std::ranges::any_of(host.mounted, [this](auto tab) {
               const auto& page = m_pages.at(tab);
               return page->dragging() || page->pointerCaptured();
             })) {
        host.tooltip->dismiss();
        return;
      }
      host.tooltip->sync(
          host.surface.renderTarget().renderer(), *host.root, host.pointerX, host.pointerY,
          {static_cast<float>(host.surface.width()), static_cast<float>(host.surface.height())}
      );
    }
    void prepare(Host& host) {
      if (host.closed
          || !host.surface.isRunning()
          || !m_model.windows().contains(host.id)
          || !m_renderer.makeCurrent(host.surface.renderTarget())) {
        return;
      }
      UiPhaseScope phase(UiPhase::Layout);
      const auto window = m_model.windows().at(host.id);
      if (!window.active) {
        return;
      }
      if (!host.root) {
        host.root =
            ui::column({.align = FlexAlign::Stretch, .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow)});
        host.root->setAnimationManager(&m_animations);
        host.surface.setSceneRoot(host.root.get());
        host.dispatcher.setSceneRoot(host.root.get());
        auto tooltip = std::make_unique<TooltipPresenter>([&host] { host.surface.requestUpdate(); });
        host.tooltip = tooltip.get();
        host.root->addChild(std::move(tooltip));
        auto divider = std::make_unique<InputArea>();
        divider->setParticipatesInLayout(false);
        divider->setZIndex(40);
        divider->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE);
        divider->setFocusable(true);
        divider->setTabFocusKey(std::format("window:{}:split", host.id));
        divider->setOnKeyDown([this, &host](const InputArea::KeyData& key) {
          if (KeySymbol::isLeft(key.sym) || KeySymbol::isRight(key.sym)) {
            m_model.resize(
                host.id, m_model.windows().at(host.id).fraction + (KeySymbol::isLeft(key.sym) ? -0.025F : 0.025F)
            );
          } else if (key.sym == XKB_KEY_Home) {
            m_model.resize(host.id, 0.5F);
          }
          host.surface.requestLayout();
        });
        host.divider = divider.get();
        auto line = ui::box({.fill = colorSpecFromRole(ColorRole::OutlineVariant), .width = 1.0F});
        divider->addChild(std::move(line));
        host.root->addChild(std::move(divider));
        auto preview = std::make_unique<InputArea>();
        preview->setParticipatesInLayout(false);
        preview->setZIndex(40);
        preview->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE);
        preview->setFocusable(true);
        preview->setTabFocusKey(std::format("window:{}:preview", host.id));
        preview->setOnKeyDown([this, &host](const InputArea::KeyData& key) {
          if (!key.pressed) {
            return;
          }
          auto& page = *m_pages.at(m_model.windows().at(host.id).active);
          if (KeySymbol::isLeft(key.sym) || KeySymbol::isRight(key.sym)) {
            page.savePreviewWidth(page.previewWidth() + (KeySymbol::isLeft(key.sym) ? 16 : -16));
          } else if (key.sym == XKB_KEY_Home) {
            page.savePreviewWidth(320);
          }
        });
        preview->setOnEnter([&host](const auto&) { host.surface.requestLayout(); });
        preview->setOnLeave([&host] { host.surface.requestLayout(); });
        host.previewDivider = preview.get();
        preview->addChild(
            ui::box({.out = &host.previewResizeLine, .fill = colorSpecFromRole(ColorRole::OutlineVariant), .width = 1})
        );
        host.root->addChild(std::move(preview));
      }
      auto& active = *m_pages.at(window.active);
      active.ensureScene();
      if (host.chromeOwner != window.active) {
        auto* previous = host.chromeOwner ? m_pages.at(host.chromeOwner).get() : nullptr;
        releaseChrome(host);
        if (previous) {
          active.reusePlacesFrom(*previous);
        }
        host.toolbar = active.takeToolbar();
        host.toolbarNode = host.toolbar.get();
        host.toolbar->setParticipatesInLayout(false);
        host.toolbar->setVisible(true);
        host.toolbar->setZIndex(40);
        host.root->addChild(std::move(host.toolbar));
        active.takeSidebar(*host.root);
        active.sidebar()->setParticipatesInLayout(false);
        active.sidebar()->setZIndex(30);
        host.chromeOwner = window.active;
      }
      rebuildStrip(host, window);
      host.splitButton->setSelected(window.secondary.has_value());
      std::vector<files::TabId> visible{window.primary};
      if (window.secondary && !active.overlay()) {
        visible.push_back(*window.secondary);
      }
      if (active.overlay()) {
        visible = {window.active};
      }
      for (auto tab : host.mounted) {
        if (!std::ranges::contains(visible, tab)) {
          auto& page = *m_pages.at(tab);
          page.hide();
          page.unmount();
        }
      }
      host.mounted = visible;
      for (auto tab : visible) {
        auto& page = *m_pages.at(tab);
        page.ensureScene();
        page.mount(*host.root);
        page.root()->setParticipatesInLayout(false);
        page.root()->setZIndex(tab == window.active ? 10 : 0);
      }
      const bool dragging = std::ranges::any_of(host.mounted, [this](auto tab) { return m_pages.at(tab)->dragging(); });
      for (const auto& [tab, button] : host.buttons) {
        const bool selected =
            tab == window.selected || (dragging && button->containsScenePoint(host.pointerX, host.pointerY));
        button->setSelected(selected);
        auto& [close, role] = host.closeButtons.at(tab);
        const auto foreground = selected ? ColorRole::OnPrimary : ColorRole::OnSurface;
        if (foreground != role) {
          close->setCustomPalette(closeTabPalette(foreground));
          role = foreground;
        }
        if (tab == window.selected && host.revealedTab != tab) {
          host.tabScroll->requestScrollIntoView(tab == window.tabs.back() ? host.newTabButton : button);
          host.revealedTab = tab;
        }
      }
      auto& renderer = host.surface.renderTarget().renderer();
      const auto width = static_cast<float>(host.surface.width()), height = static_cast<float>(host.surface.height());
      host.root->arrange(renderer, {0, 0, width, height});
      host.strip->arrange(renderer, {0, 0, width, 40});
      host.toolbarNode->setVisible(!active.overlay());
      active.sidebar()->setVisible(!active.overlay());
      if (active.overlay()) {
        host.divider->setVisible(false);
        host.previewDivider->setVisible(false);
        active.setPreviewBounds(std::nullopt);
        active.prepare({0, 40, width, height - 40}, true, host.root.get());
        syncTooltip(host);
        return;
      }
      host.toolbarNode->arrange(renderer, {0, 40, width, 54});
      const float top = 94 + kWorkspaceInset;
      const float bodyHeight = std::max(1.0F, height - top - kWorkspaceInset);
      const float sidebar = active.sidebarWidth();
      active.sidebar()->arrange(
          renderer, {kWorkspaceInset, top, std::max(0.0F, sidebar - kWorkspaceInset), bodyHeight}
      );
      const float left = kWorkspaceInset + sidebar;
      const float right = width - kWorkspaceInset;
      const bool split = visible.size() == 2;
      const float previewBudget = right - left - kMinimumPaneWidth * (split ? 2.0F : 1.0F);
      const float previewWidth = active.previewVisible() && previewBudget >= kMinimumPreviewWidth
          ? std::min(static_cast<float>(host.previewDragWidth.value_or(active.previewWidth())), previewBudget)
          : 0;
      host.previewDivider->setVisible(previewWidth > 0);
      host.previewResizeLine->setFill(colorSpecFromRole(
          host.previewDragWidth || host.previewDivider->focused() ? ColorRole::Primary
              : host.previewDivider->hovered()                    ? ColorRole::OnSurfaceVariant
                                                                  : ColorRole::OutlineVariant
      ));
      host.previewDivider->arrange(renderer, {right - previewWidth - 3, top, 6, bodyHeight});
      host.previewDivider->children().front()->arrange(renderer, {2.5F, 0, 1, bodyHeight});
      for (auto tab : visible) {
        m_pages.at(tab)->setPreviewBounds(
            tab == window.active && previewWidth > 0
                ? std::optional(LayoutRect{right - previewWidth, top, previewWidth, bodyHeight})
                : std::nullopt
        );
      }
      const float content = std::max(1.0F, right - left - previewWidth);
      host.browsingBounds = {left, top, content, bodyHeight};
      const float minimumPane = std::min(kMinimumPaneWidth, content * 0.5F);
      const float first = split ? std::clamp(content * window.fraction, minimumPane, content - minimumPane) : content;
      host.divider->setVisible(split);
      host.divider->arrange(renderer, {left + first - 3, top, 6, bodyHeight});
      host.divider->children().front()->arrange(renderer, {2.5F, 0, 1.0F, bodyHeight});
      for (std::size_t index = 0; index < visible.size(); ++index) {
        const auto tab = visible[index];
        m_pages.at(tab)->prepare(
            {left + (index ? first : 0), top, index ? content - first : first, bodyHeight}, tab == window.active,
            host.root.get(), index == 0, index == visible.size() - 1 && previewWidth == 0
        );
      }
      active.layoutNavigation(renderer);
      syncTooltip(host);
    }
    WaylandConnection& m_connection;
    RenderContext& m_renderer;
    TextInputService& m_input;
    files::WaylandTextClipboard& m_clipboard;
    files::BrowserSettings m_defaults;
    AnimationManager m_animations;
    files::ViewPreferenceWriter m_preferences;
    files::PinService m_pins;
    files::FileIconCache m_icons;
    files::TabWorkspace m_model;
    std::map<files::TabId, std::unique_ptr<BrowserPage>> m_pages;
    std::map<files::WindowId, std::unique_ptr<Host>> m_hosts;
    files::WindowId m_focused = 0;
    std::optional<files::TabId> m_cacheClearOwner;
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
    application.start(initialLocation);
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
        [&application] { return application.running(); }
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
