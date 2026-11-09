#include "app/page/settings_controller.h"

#include "app/page/active_dialog.h"
#include "app/page/archive_controller.h"
#include "app/page/menu_controller.h"
#include "app/page/page_context.h"
#include "config/view_preference_writer.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "thumbnails/thumbnail_service.h"
#include "ui/browser/action_shortcuts.h"
#include "ui/controls/browser_settings_view.h"
#include "ui/controls/context_menu.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_seat.h"

#include <utility>
#include <vector>

SettingsController::SettingsController(
    PageContext& page, files::ViewPreferenceWriter& preferences, files::ThumbnailService& thumbnails,
    InputDispatcher& dispatcher, ArchiveController& archive, MenuController& menus,
    std::function<void()> clearThumbnails
)
    : m_page(page), m_preferences(preferences), m_thumbnails(thumbnails), m_dispatcher(dispatcher), m_archive(archive),
      m_menus(menus), m_clearThumbnails(std::move(clearThumbnails)) {}

void SettingsController::preferencesChanged(const std::string& error) {
  m_error = error;
  m_pending = m_preferences.settings();
  m_dirty = true;
}

void SettingsController::onFrame() {
  if (std::exchange(m_dirty, false)) {
    if (m_pending) {
      m_page.applySettings(m_pending->browser, !m_error.empty());
    }
    if (m_view) {
      sync();
      m_page.surface().requestLayout();
    }
  }
}

void SettingsController::sync() {
  const auto& inherited = m_preferences.inheritedSettings();
  m_view->sync(
      m_page.settings(), inherited ? inherited->browser : files::BrowserSettings{},
      m_preferences.busy() || m_thumbnails.clearing(), m_error.empty() ? m_thumbnails.error() : m_error,
      m_thumbnails.clearing()
  );
}

void SettingsController::deferClose() {
  const std::weak_ptr<int> lifetime = m_lifetime;
  DeferredCall::callLater([this, lifetime] {
    if (!lifetime.expired()) {
      close();
    }
  });
}

void SettingsController::close() {
  if (!m_view) {
    return;
  }
  m_page.closeMenu();
  m_viewLifetime.reset();
  m_dispatcher.cancelPointerCapture();
  m_page.focusFileList();
  auto* settings = std::exchange(m_view, nullptr);
  auto removed = m_page.sceneRoot().removeChild(settings);
  m_page.surface().requestLayout();
}

void SettingsController::toggle() {
  if (m_view) {
    deferClose();
    return;
  }
  m_archive.close();
  // Settings is closed, so this closes the active dialog, drags and location editing.
  m_page.prepareOverlay();
  m_viewLifetime = std::make_shared<int>(0);
  auto root = std::make_unique<BrowserSettingsView>(
      [this](files::OverrideEdit edit) {
        m_preferences.submit({edit});
        m_page.surface().requestLayout();
      },
      [this] { m_preferences.resetAll(); }, [this] { deferClose(); }, [this] { m_clearThumbnails(); }
  );
  root->setParticipatesInLayout(false);
  root->setZIndex(1000);
  root->setPopupContext(this);
  m_view = root.get();
  sync();
  m_page.sceneRoot().addChild(std::move(root));
  m_dispatcher.cancelPointerCapture();
  m_dispatcher.setFocus(m_dispatcher.firstTabFocusUnder(m_view));
  m_page.surface().requestLayout();
}

bool SettingsController::key(const KeyboardEvent& event) {
  if (!m_view) {
    return false;
  }
  if (event.pressed
      && (KeySymbol::isEscape(event.sym)
          || files::shortcutMatches(files::BrowserAction::Settings, event.sym, event.modifiers))) {
    deferClose();
  } else if (event.pressed && event.sym == XKB_KEY_q && event.modifiers == KeyMod::Ctrl) {
    m_page.surface().close();
  } else if (event.pressed && KeySymbol::isTab(event.sym)) {
    (void)m_dispatcher.cycleTabFocusInSubtree(m_view, (event.modifiers & KeyMod::Shift) != 0);
  } else {
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  }
  m_page.surface().requestLayout();
  return true;
}

void SettingsController::layout(Renderer& renderer, LayoutRect bounds, bool compact) {
  if (m_view) {
    m_view->setCompact(compact);
    m_view->arrange(renderer, bounds);
  }
}

void SettingsController::revealFocus(InputArea* focused) {
  if (m_view) {
    m_view->reveal(focused);
    m_page.surface().requestLayout();
  }
}

void SettingsController::openSelectDropdown(const DropdownRequest& request, DropdownCallbacks callbacks) {
  if (!m_view) {
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
  const std::weak_ptr<int> lifetime = m_viewLifetime;
  m_menus.showContextMenu(
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
  if (auto* menu = m_menus.menu()) {
    menu->setHighlightedIndex(request.selectedIndex);
    m_menus.revealHighlight();
  }
  m_page.activeDialog().bind(ActiveDialog::Kind::Dropdown, [lifetime, dismiss = callbacks.onDismiss] {
    if (!lifetime.expired() && dismiss) {
      dismiss();
    }
  });
}

void SettingsController::closeSelectDropdown() { m_page.dismissMenu(); }

bool SettingsController::isSelectDropdownOpen() const {
  return m_page.menuSurface() && m_page.activeDialog().kind() == ActiveDialog::Kind::Dropdown;
}
