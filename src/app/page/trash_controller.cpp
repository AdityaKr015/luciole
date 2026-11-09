#include "app/page/trash_controller.h"

#include "app/page/active_dialog.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "core/ui_phase.h"
#include "fs/gio_ptr.h"
#include "operations/empty_trash_service.h"
#include "operations/restore_service.h"
#include "operations/trash_service.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/builders.h"
#include "ui/controls/file_error_text.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <utility>

TrashController::TrashController(
    PageContext& page, files::TrashService& trash, files::EmptyTrashService& emptyTrash, files::RestoreService& restore
)
    : m_page(page), m_trash(trash), m_emptyTrash(emptyTrash), m_restore(restore) {}

bool TrashController::canTrashItems(const std::vector<files::DirectoryEntry>& targets) const {
  return m_page.actionState(files::BrowserAction::Trash, m_page.requestFor(targets)).enabled;
}

void TrashController::trashItems(const std::vector<files::DirectoryEntry>& targets) {
  if (!canTrashItems(targets) || !m_page.targetsCurrent(targets)) {
    m_page.actionError("This selection cannot be moved to Trash right now.");
    return;
  }
  m_page.cancelDrag();
  std::vector<files::Location> sources;
  sources.reserve(targets.size());
  for (const auto& target : targets) {
    sources.push_back(target.location);
  }
  if (!m_trash.prepare(*m_page.browser().model().snapshot().location, std::move(sources))) {
    m_page.actionError("Another Trash operation is still finishing.");
  }
}

void TrashController::restore(const std::vector<files::DirectoryEntry>& targets) {
  m_page.cancelDrag();
  m_page.setActionMessage({});
  if (!m_restore.start(targets)) {
    m_page.actionError("Could not start restoring the selection.");
  }
}

void TrashController::restoreChanged() {
  if (!m_restore.busy()) {
    const auto& results = m_restore.results();
    const auto count = [&](files::RestoreOutcome outcome) {
      return std::ranges::count(results, outcome, &files::RestoreResult::outcome);
    };
    const auto restored = count(files::RestoreOutcome::Restored);
    std::string message = std::format("{} item{} restored", restored, restored == 1 ? "" : "s");
    for (const auto& [outcome, label] :
         {std::pair{files::RestoreOutcome::Skipped, "skipped"}, std::pair{files::RestoreOutcome::Failed, "failed"},
          std::pair{files::RestoreOutcome::Cancelled, "cancelled"},
          std::pair{files::RestoreOutcome::Unconfirmed, "could not be confirmed"}}) {
      if (const auto amount = count(outcome); amount != 0) {
        message += std::format(" · {} {}", amount, label);
      }
    }
    const auto error = std::ranges::find_if(results, [](const auto& item) { return item.error.has_value(); });
    if (error != results.end()) {
      message += std::format(" · {}: {}", error->displayName, files::fileErrorText(error->error));
    }
    m_page.setActionMessage(std::move(message));
    m_page.browser().refresh();
  }
}

void TrashController::openEmptyTrash(std::optional<std::vector<files::DirectoryEntry>> selection) {
  if (!m_page.fileJobsIdle()) {
    m_page.actionError("Another file operation is still finishing.");
    return;
  }
  m_page.cancelDrag();
  auto& surface = m_page.surface();
  const float width = std::min(420.0F, static_cast<float>(surface.width()) - 32.0F);
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
               .onClick = [this] { m_page.dismissMenu(); }}
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
  const float height = root->measure(surface.renderTarget().renderer(), constraints).height;
  m_page.showPopup(
      std::move(root), nullptr, {(surface.width() - width) * 0.5F, (surface.height() - height) * 0.5F, 1, 1}, width,
      height, false
  );
  if (m_page.menuSurface() != nullptr) {
    m_page.activeDialog().bind(ActiveDialog::Kind::EmptyTrash, [this] {
      if (std::exchange(m_emptyTrashDialog, false)) {
        m_emptyTrash.cancel();
      }
      m_emptyTrashMessage = nullptr;
      m_emptyTrashSize = nullptr;
      m_emptyTrashConfirm = nullptr;
    });
    m_emptyTrashDialog = true;
    m_selectedDelete = selection.has_value();
    m_emptyTrashMessage = message;
    m_emptyTrashSize = size;
    m_emptyTrashConfirm = confirm;
    m_page.menuDispatcher().setFocus(cancel->inputArea());
    if (!(selection ? m_emptyTrash.prepareSelection(std::move(*selection)) : m_emptyTrash.prepare())) {
      message->setText("Another Trash deletion is still finishing.");
    }
  }
}

void TrashController::confirmEmptyTrash() {
  if (!m_emptyTrashDialog || !m_emptyTrash.empty()) {
    return;
  }
  m_emptyTrashConfirm->setEnabled(false);
  m_emptyTrashDialog = false;
  m_page.dismissMenu();
}

void TrashController::onFrame() {
  if (std::exchange(m_trashDirty, false)) {
    syncTrash();
  }
  if (std::exchange(m_emptyTrashDirty, false)) {
    syncEmptyTrash();
  }
}

void TrashController::syncEmptyTrash() {
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
      m_emptyTrashMessage->setText("Could not read Trash: " + files::fileErrorText(snapshot.error));
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
    m_page.menuSurface()->requestLayout();
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
    const auto error = std::ranges::find_if(snapshot.results, [](const auto& item) { return item.error.has_value(); });
    if (error != snapshot.results.end()) {
      message += std::format(" · {}: {}", error->displayName, files::fileErrorText(error->error));
    }
    m_page.actionError(std::move(message));
    auto& browser = m_page.browser();
    if (const auto& location = browser.model().snapshot().location; location && location->isTrash()) {
      browser.open(files::Location::trash());
    }
  } else if (snapshot.status == files::EmptyTrashStatus::Error) {
    m_page.actionError(
        std::string(m_selectedDelete ? "Could not delete selected items: " : "Could not empty Trash: ")
        + files::fileErrorText(snapshot.error)
    );
  }
}

void TrashController::syncTrash() {
  const auto& snapshot = m_trash.snapshot();
  if (snapshot.status == files::TrashStatus::Ready) {
    (void)m_trash.trash();
  } else if (snapshot.status == files::TrashStatus::Error) {
    m_page.actionError("Could not move to Trash: " + files::fileErrorText(snapshot.error));
  } else if (snapshot.status == files::TrashStatus::Cancelled) {
    m_page.actionError("Move to Trash cancelled.");
  } else if (snapshot.status == files::TrashStatus::Finished) {
    const auto done = std::ranges::count(snapshot.results, files::TrashOutcome::Trashed, &files::TrashResult::outcome);
    const auto failed = std::ranges::count(snapshot.results, files::TrashOutcome::Failed, &files::TrashResult::outcome);
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
        std::ranges::find_if(snapshot.results, [](const auto& result) { return result.error.has_value(); });
    if (error != snapshot.results.end()) {
      message +=
          std::format(" · {}: {}", error->source.displayName().value_or("Item"), files::fileErrorText(*error->error));
    }
    m_page.actionError(std::move(message));
    m_page.browser().refresh();
  }
}

void TrashController::layoutDialog() {
  if (!m_emptyTrashDialog) {
    return;
  }
  auto& surface = m_page.surface();
  auto* menuSurface = m_page.menuSurface();
  LayoutConstraints constraints;
  constraints.setExactWidth(static_cast<float>(menuSurface->width()));
  const auto height = static_cast<std::uint32_t>(std::ceil(
      std::clamp(
          m_page.activeDialog().root()->measure(menuSurface->renderTarget().renderer(), constraints).height, 1.0F,
          static_cast<float>(surface.height()) - 64.0F
      )
  ));
  if (height != menuSurface->height()) {
    const auto width = menuSurface->width();
    (void)menuSurface->resize(width, height, false);
    (void)menuSurface->repositionAnchor(
        {.anchorX = static_cast<int>((surface.width() - width) / 2),
         .anchorY = static_cast<int>((surface.height() - height) / 2),
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
