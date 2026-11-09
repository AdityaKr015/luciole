#include "app/page/batch_rename_controller.h"

#include "app/page/active_dialog.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "clipboard/text_clipboard.h"
#include "operations/batch_rename_service.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/batch_rename_dialog.h"
#include "ui/controls/file_error_text.h"
#include "ui/controls/input.h"
#include "ui/controls/virtual_grid_view.h"
#include "wayland/popup_surface.h"

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

BatchRenameController::BatchRenameController(PageContext& page, files::BatchRenameService& service)
    : m_page(page), m_service(service) {}

void BatchRenameController::open(const std::vector<files::DirectoryEntry>& targets) {
  if (!m_page.actionState(files::BrowserAction::Rename, m_page.requestFor(targets)).enabled
      || !m_page.targetsCurrent(targets)) {
    m_page.actionError("This selection cannot be renamed right now.");
    return;
  }
  auto root = std::make_unique<BatchRenameDialog>(
      targets.size(), [this](files::BatchRenameOptions options) { (void)m_service.preview(std::move(options)); },
      [this] { (void)m_service.apply(); },
      [this] {
        if (m_service.applying()) {
          m_service.cancel();
        } else {
          m_page.dismissMenu();
        }
      },
      [this] { m_page.dismissMenu(); }, [this] { m_page.clipboard().setClipboardText(m_service.snapshot().report); },
      [this] {
        if (auto* surface = m_page.menuSurface()) {
          surface->requestUpdate();
        }
      }
  );
  auto* dialog = root.get();
  auto& surface = m_page.surface();
  const float width = std::min(760.0F, static_cast<float>(surface.width()) - 32.0F);
  const float height = std::min(560.0F, static_cast<float>(surface.height()) - 64.0F);
  root->setSize(width, height);
  m_page.showPopup(
      std::move(root), nullptr, {(surface.width() - width) * 0.5F, (surface.height() - height) * 0.5F, 1, 1}, width,
      height, false
  );
  if (m_page.menuSurface()) {
    m_dialog = dialog;
    m_page.activeDialog().bind(ActiveDialog::Kind::BatchRename, [this] {
      if (m_dialog) {
        m_service.cancel();
        m_dialog = nullptr;
        m_page.focusFileList();
      }
    });
    m_reported = false;
    m_page.setDialogSize(width, height);
    m_page.menuDispatcher().setFocusChangeCallback([this](InputArea*, InputArea* focused) {
      if (m_dialog) {
        m_dialog->reveal(focused);
        m_page.menuSurface()->requestLayout();
      }
    });
    m_page.menuDispatcher().setFocus(dialog->firstInput()->inputArea());
    dialog->firstInput()->selectAll();
    if (!m_service.prepare(targets)) {
      m_page.actionError("Another rename is still finishing.");
      m_page.dismissMenu();
    }
  }
}

void BatchRenameController::progressTick() { m_dirty = m_service.applying(); }

void BatchRenameController::onFrame() {
  if (std::exchange(m_dirty, false)) {
    sync();
  }
}

void BatchRenameController::sync() {
  const auto& snapshot = m_service.snapshot();
  if (m_dialog) {
    m_dialog->sync(snapshot, m_service.completed(), m_page.clipboard().available());
    m_page.menuSurface()->requestLayout();
  }
  if (snapshot.phase == files::BatchRenamePhase::Finished && !m_reported) {
    m_reported = true;
    const auto count =
        std::ranges::count(*snapshot.results, files::BatchRenameOutcome::Renamed, &files::BatchRenameResult::outcome);
    std::string message = std::format("{} items renamed.", count);
    const auto problem =
        std::ranges::find_if(*snapshot.results, [](const auto& result) { return result.error.has_value(); });
    if (problem != snapshot.results->end()) {
      message += " " + files::fileErrorText(problem->error);
    }
    m_page.actionError(std::move(message));
    auto& browser = m_page.browser();
    if (snapshot.parent == browser.model().snapshot().location) {
      m_reveal.clear();
      for (const auto& result : *snapshot.results) {
        if (result.actual) {
          m_reveal.push_back(*result.actual);
        }
      }
      browser.refresh();
      m_revealContext = browser.contextSerial();
    }
  } else if (snapshot.phase == files::BatchRenamePhase::Error && !m_dialog) {
    m_page.actionError("Could not rename: " + files::fileErrorText(snapshot.error));
  }
}

void BatchRenameController::pointer(float x, float y, bool hover) {
  if (m_dialog) {
    m_dialog->tooltipPointer(x, y, hover);
  }
}

void BatchRenameController::layoutDialog(float width) {
  if (m_dialog) {
    m_dialog->setSize(width, std::min(560.0F, static_cast<float>(m_page.surface().height()) - 64.0F));
  }
}

void BatchRenameController::syncTooltip(Renderer& renderer) {
  if (m_dialog) {
    m_dialog->syncTooltip(renderer);
  }
}

void BatchRenameController::revealRenamed(VirtualGridView& list) {
  auto& browser = m_page.browser();
  const auto& model = browser.model();
  if (!m_reveal.empty() && model.snapshot().status != files::DirectoryStatus::Loading) {
    if (m_revealContext == browser.contextSerial()) {
      std::unordered_set<std::string> uris;
      for (const auto& target : m_reveal) {
        uris.insert(target.uri());
      }
      std::vector<files::EntryId> ids;
      for (const auto id : model.rows()) {
        if (const auto* entry = model.entry(id); entry && uris.contains(entry->location.uri())) {
          ids.push_back(id);
        }
      }
      browser.model().replaceSelection(ids);
      if (!ids.empty()) {
        const auto found = std::ranges::find(model.rows(), ids.front());
        list.scrollToIndex(static_cast<std::size_t>(found - model.rows().begin()));
      }
    }
    // Staging emits several monitor events. Keep reconciling while the results
    // are open, then leave the final selection under normal user control.
    if (!m_dialog || m_revealContext != browser.contextSerial()) {
      m_reveal.clear();
    }
  }
}
