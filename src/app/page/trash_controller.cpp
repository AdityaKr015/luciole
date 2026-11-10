#include "app/page/trash_controller.h"

#include "app/operation_hub.h"
#include "app/page/active_dialog.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "core/ui_phase.h"
#include "fs/gio_ptr.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/builders.h"
#include "ui/controls/file_error_text.h"
#include "ui/dialogs/dialog_chrome.h"
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

TrashController::TrashController(PageContext& page) : m_page(page) {}

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
  const auto job = m_page.jobs().create<files::TrashService>();
  if (!job->service<files::TrashService>().prepare(*m_page.browser().model().snapshot().location, std::move(sources))) {
    m_page.actionError("Another Trash operation is still finishing.");
  }
}

void TrashController::restore(const std::vector<files::DirectoryEntry>& targets) {
  m_page.cancelDrag();
  m_page.setActionMessage({});
  if (!m_page.jobs().create<files::RestoreService>()->service<files::RestoreService>().start(targets)) {
    m_page.actionError("Could not start restoring the selection.");
  }
}

void TrashController::restoreChanged(OperationJob& job) {
  const auto& restore = job.service<files::RestoreService>();
  if (!restore.busy() && job.takeResult()) {
    const auto& results = restore.results();
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
  if (m_page.jobs().busy()) {
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
      {.align = FlexAlign::Stretch},
      files::dialogHeader(
          selection ? "Delete selected items permanently?" : "Empty Trash?", "trash", [this] { m_page.dismissMenu(); }
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
  files::applyDialogStyle(*root);
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
      if (const auto job = std::exchange(m_emptyTrash, nullptr)) {
        job->cancel();
      }
      m_emptyTrashMessage = nullptr;
      m_emptyTrashSize = nullptr;
      m_emptyTrashConfirm = nullptr;
    });
    m_emptyTrash = m_page.jobs().create<files::EmptyTrashService>(JobScope::Page);
    m_emptyTrashMessage = message;
    m_emptyTrashSize = size;
    m_emptyTrashConfirm = confirm;
    m_page.menuDispatcher().setFocus(cancel->inputArea());
    auto& emptyTrash = m_emptyTrash->service<files::EmptyTrashService>();
    if (!(selection ? emptyTrash.prepareSelection(std::move(*selection)) : emptyTrash.prepare())) {
      message->setText("Another Trash deletion is still finishing.");
    }
  }
}

void TrashController::confirmEmptyTrash() {
  if (!m_emptyTrash || !m_emptyTrash->service<files::EmptyTrashService>().empty()) {
    return;
  }
  m_page.jobs().follow(*m_emptyTrash);
  m_emptyTrashConfirm->setEnabled(false);
  m_emptyTrash.reset();
  m_page.dismissMenu();
}

void TrashController::onFrame() {
  if (std::exchange(m_emptyTrashDirty, false)) {
    syncEmptyTrash();
  }
}

void TrashController::emptyTrashChanged(OperationJob& job) {
  if (&job == m_emptyTrash.get()) {
    m_emptyTrashDirty = true;
    return;
  }
  const auto& snapshot = job.service<files::EmptyTrashService>().snapshot();
  if (snapshot.status == files::EmptyTrashStatus::Finished && job.takeResult()) {
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
  } else if (snapshot.status == files::EmptyTrashStatus::Error && job.takeResult()) {
    m_page.actionError(
        std::string(snapshot.selection ? "Could not delete selected items: " : "Could not empty Trash: ")
        + files::fileErrorText(snapshot.error)
    );
  }
}

void TrashController::syncEmptyTrash() {
  if (!m_emptyTrash || m_emptyTrashMessage == nullptr) {
    return;
  }
  const auto& emptyTrash = m_emptyTrash->service<files::EmptyTrashService>();
  const auto& snapshot = emptyTrash.snapshot();
  if (snapshot.status == files::EmptyTrashStatus::Ready) {
    const auto count = snapshot.results.size();
    m_emptyTrashMessage->setText(
        count == 0 ? "Trash is already empty."
                   : std::format(
                         "Permanently delete {} item{} and their contents? This cannot be undone.", count,
                         count == 1 ? "" : "s"
                     )
    );
    m_emptyTrashConfirm->setEnabled(emptyTrash.canEmpty());
  } else if (snapshot.status == files::EmptyTrashStatus::Error) {
    m_emptyTrashMessage->setText("Could not read Trash: " + files::fileErrorText(snapshot.error));
    m_emptyTrashConfirm->setEnabled(false);
  }
  std::string sizeText = "Size: calculating…";
  if (snapshot.size) {
    const files::GString value(g_format_size_full(*snapshot.size, G_FORMAT_SIZE_IEC_UNITS));
    sizeText = std::format("Size: {}{}", snapshot.sizeIncomplete ? "at least " : "", value.get());
  } else if (!emptyTrash.busy()) {
    sizeText = "Size unavailable";
  }
  m_emptyTrashSize->setText(sizeText);
  m_page.menuSurface()->requestLayout();
}

void TrashController::trashChanged(OperationJob& job) {
  const auto& trash = job.service<files::TrashService>();
  const auto& snapshot = trash.snapshot();
  // OperationHub::update() starts a prepared move.
  if (trash.busy() || trash.canTrash() || !job.takeResult()) {
    return;
  }
  if (snapshot.status == files::TrashStatus::Error) {
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
  if (!m_emptyTrash) {
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
