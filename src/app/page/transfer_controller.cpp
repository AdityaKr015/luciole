#include "app/page/transfer_controller.h"

#include "app/page/active_dialog.h"
#include "app/page/page_context.h"
#include "browser/browser_controller.h"
#include "browser/location_presentation.h"
#include "core/ui_phase.h"
#include "fs/gio_ptr.h"
#include "operations/archive_service.h"
#include "operations/batch_rename_service.h"
#include "operations/empty_trash_service.h"
#include "operations/permission_service.h"
#include "operations/restore_service.h"
#include "operations/transfer_service.h"
#include "operations/trash_service.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "ui/builders.h"
#include "ui/controls/file_error_text.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <utility>
#include <vector>

TransferController::TransferController(
    PageContext& page, files::TransferService& transfers, files::ArchiveService& archives,
    files::RestoreService& restore, files::TrashService& trash, files::EmptyTrashService& emptyTrash,
    files::BatchRenameService& batchRenamer, files::PermissionService& permissions, std::function<void()> progressTick
)
    : m_page(page), m_transfers(transfers), m_archives(archives), m_restore(restore), m_trash(trash),
      m_emptyTrash(emptyTrash), m_batchRenamer(batchRenamer), m_permissions(permissions),
      m_progressTick(std::move(progressTick)) {}

void TransferController::paste(const files::ActionRequest& request) {
  m_pendingFilePaste = true;
  const std::weak_ptr<int> lifetime = m_lifetime;
  m_page.clipboard().requestFiles([this, lifetime, request](std::optional<files::ClipboardFiles> selection) {
    if (lifetime.expired()) {
      return;
    }
    const bool pending = std::exchange(m_pendingFilePaste, false);
    if (!pending || !m_page.visible() || !m_page.requestCurrent(request, files::ActionScope::Location)) {
      return;
    }
    if (!selection) {
      m_page.actionError("The clipboard changed or its file data could not be read.");
      return;
    }
    if (m_page.operationsBusy()) {
      return;
    }
    if (selection->payload.cut) {
      if (!m_page.clipboard().claimMove(selection->revision)) {
        return;
      }
      m_clipboardTransfer = *selection;
    }
    if (!startPayloadTransfer(
            selection->payload, *request.folder,
            selection->payload.cut ? files::TransferAction::Move : files::TransferAction::Copy
        )) {
      if (const auto claimed = std::exchange(m_clipboardTransfer, std::nullopt)) {
        m_page.clipboard().finishMove(*claimed, {});
      }
    }
  });
}

void TransferController::transferTo(
    const files::ActionRequest& request, const files::Location& destination, bool copy
) {
  if (!m_page.requestCurrent(request, files::ActionScope::Targets)) {
    return;
  }
  std::vector<files::Location> sources;
  sources.reserve(request.targets.size());
  for (const auto& target : request.targets) {
    sources.push_back(target.location);
  }
  if (!sources.empty() && !m_transfers.busy()) {
    (void)m_transfers.start(
        {std::move(sources), destination, copy ? files::TransferAction::Copy : files::TransferAction::Move}
    );
  }
}

void TransferController::receiveDrop(
    files::FilePayload payload, const files::Location& destination, files::TransferAction action,
    files::DropCompletion finish
) {
  m_dropCompletion = std::move(finish);
  if (!startPayloadTransfer(std::move(payload), destination, action)) {
    std::exchange(m_dropCompletion, {})(false);
  }
}

void TransferController::changed() {
  if (!m_archives.busy() && !m_transfers.busy()) {
    const auto& results = m_transfers.results();
    const auto done = std::ranges::count_if(results, [](const auto& item) {
      return item.outcome == files::TransferOutcome::Moved || item.outcome == files::TransferOutcome::Copied;
    });
    std::string message = std::format(
        "{} item{} {}", done, done == 1 ? "" : "s",
        m_transfers.action() == files::TransferAction::Copy ? "copied" : "moved"
    );
    if (const auto error = std::ranges::find_if(results, [](const auto& item) { return item.error.has_value(); });
        error != results.end()) {
      message +=
          std::format(" · {}: {}", error->source.displayName().value_or("Item"), files::fileErrorText(*error->error));
    }
    m_page.setActionMessage(std::move(message));
    if (const auto selection = std::exchange(m_clipboardTransfer, std::nullopt)) {
      m_page.clipboard().finishMove(*selection, results);
    }
    if (auto finish = std::exchange(m_dropCompletion, {})) {
      finish(!results.empty() && std::ranges::all_of(results, [](const auto& item) {
        return item.outcome == files::TransferOutcome::Moved || item.outcome == files::TransferOutcome::Copied;
      }));
    }
    m_page.browser().refresh();
  }
}

void TransferController::syncConflict() {
  if (!m_page.popupAvailable()) {
    return;
  }
  const bool archive = m_archives.conflict().has_value();
  const bool restore = m_restore.conflict().has_value();
  const auto conflict = restore ? m_restore.conflict() : archive ? m_archives.conflict() : m_transfers.conflict();
  if (m_activeConflict && !conflict) {
    m_page.dismissMenu();
    return;
  }
  if (!conflict || m_page.menuSurface() != nullptr) {
    return;
  }
  auto& surface = m_page.surface();
  const float width = std::min(520.0F, static_cast<float>(surface.width()) - 32.0F);
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
            {.text = title, .fontSize = Style::fontSizeCaption, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
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
  const auto applyToAll = std::make_shared<bool>(false);
  const auto choose = [this, archive, restore, applyToAll, id = conflict->id](files::TransferConflictChoice choice) {
    if (restore ? m_restore.resolve(id, choice)
            : archive
            ? m_archives.resolve(id, choice)
            : m_transfers.resolve(id, choice, *applyToAll && choice != files::TransferConflictChoice::Cancel)) {
      m_activeConflict.reset();
      m_page.dismissMenu();
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
          facts("Existing", conflict->destinationDirectory, conflict->destinationSize, conflict->destinationModified)
      ),
      ui::label(
          {.text = restore                      ? "Folder replacement is unavailable. Keep both or skip this item."
               : conflict->canMerge && !archive ? "Merge keeps existing contents and asks about conflicting "
                                                  "children. Replace removes the whole existing folder."
                                                : "Replace removes the existing folder and its contents.",
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxWidth = width - 24.0F,
           .maxLines = 2,
           .visible = conflict->destinationDirectory,
           .participatesInLayout = conflict->destinationDirectory}
      ),
      ui::row(
          {.align = FlexAlign::Center,
           .gap = Style::spaceSm,
           .visible = !archive && !restore && conflict->canApplyToAll,
           .participatesInLayout = !archive && !restore && conflict->canApplyToAll},
          ui::toggle({.onChange = [applyToAll](bool checked) { *applyToAll = checked; }}),
          ui::label(
              {.text = conflict->scopeLabel,
               .fontSize = Style::fontSizeCaption,
               .maxWidth = width - 96.0F,
               .maxLines = 2}
          )
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
              {.text = "Merge",
               .variant = ButtonVariant::Ghost,
               .visible = !archive && !restore && conflict->canMerge,
               .participatesInLayout = !archive && !restore && conflict->canMerge,
               .onClick = [choose] { choose(files::TransferConflictChoice::Merge); }}
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
      root->measure(surface.renderTarget().renderer(), constraints).height, static_cast<float>(surface.height()) - 64.0F
  );
  m_page.showPopup(
      std::move(root), nullptr, {(surface.width() - width) * 0.5F, (surface.height() - height) * 0.5F, 1, 1}, width,
      height, false
  );
  if (m_page.menuSurface() != nullptr) {
    m_page.activeDialog().bind(ActiveDialog::Kind::TransferConflict, [this] {
      if (const auto id = std::exchange(m_activeConflict, std::nullopt)) {
        (void)(m_conflictRestore       ? m_restore.resolve(*id, files::TransferConflictChoice::Cancel)
                   : m_conflictArchive ? m_archives.resolve(*id, files::TransferConflictChoice::Cancel)
                                       : m_transfers.resolve(*id, files::TransferConflictChoice::Cancel));
      }
    });
    m_activeConflict = conflict->id;
    m_conflictArchive = archive;
    m_conflictRestore = restore;
    m_page.menuDispatcher().setFocus(keepBoth->inputArea());
  } else {
    (void)(restore       ? m_restore.resolve(conflict->id, files::TransferConflictChoice::Cancel)
               : archive ? m_archives.resolve(conflict->id, files::TransferConflictChoice::Cancel)
                         : m_transfers.resolve(conflict->id, files::TransferConflictChoice::Cancel));
  }
}

void TransferController::retry() {
  if (m_page.operationsBusy() || m_transfers.retryable() == 0) {
    return;
  }
  if (m_transfers.action() != files::TransferAction::Move || !m_page.clipboard().hasFiles()) {
    (void)m_transfers.retryFailed();
    return;
  }
  const auto job = m_transfers.jobId();
  const std::weak_ptr<int> lifetime = m_lifetime;
  m_page.clipboard().requestFiles([this, lifetime, job](std::optional<files::ClipboardFiles> selection) {
    if (lifetime.expired() || m_transfers.jobId() != job || m_page.operationsBusy()) {
      return;
    }
    if (selection
        && selection->payload.cut
        && std::ranges::any_of(selection->payload.locations, [this](const auto& location) {
             return std::ranges::any_of(m_transfers.results(), [&](const auto& result) {
               return result.source == location;
             });
           })) {
      if (!m_page.clipboard().claimMove(selection->revision)) {
        return;
      }
      m_clipboardTransfer = *selection;
    }
    if (!m_transfers.retryFailed()) {
      if (const auto claimed = std::exchange(m_clipboardTransfer, std::nullopt)) {
        m_page.clipboard().finishMove(*claimed, {});
      }
    }
  });
}

bool TransferController::startPayloadTransfer(
    files::FilePayload payload, const files::Location& destination, files::TransferAction action
) {
  if (payload.locations.empty()
      || !destination.nativePath()
      || std::ranges::any_of(payload.locations, [](const auto& location) { return !location.nativePath(); })) {
    m_page.actionError("File transfers currently support native files and folders outside Trash.");
    return false;
  }
  m_page.setActionMessage({});
  if (!m_transfers.start({std::move(payload.locations), destination, action})) {
    m_page.actionError("Another file transfer is still finishing.");
    return false;
  }
  return true;
}

void TransferController::updateOperationProgress() {
  if (m_permissions.busy()
      || m_batchRenamer.applying()
      || m_archives.busy()
      || m_transfers.busy()
      || m_restore.busy()
      || m_trash.busy()
      || m_emptyTrash.busy()) {
    m_operationProgress.startRepeating(std::chrono::milliseconds(100), [this] { m_progressTick(); });
  } else {
    m_operationProgress.stop();
  }
}

std::unique_ptr<Button> TransferController::buildRetryButton() {
  return ui::button(
      {.out = &m_retryTransfer,
       .text = "Retry failed",
       .variant = ButtonVariant::Ghost,
       .tooltip = "Retry failed items without repeating completed transfers",
       .visible = false,
       .participatesInLayout = false,
       .onClick = [this] { retry(); }}
  );
}

std::unique_ptr<Button> TransferController::buildCancelButton() {
  return ui::button(
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
  );
}

void TransferController::syncOperationButtons(bool selectedTrashDelete) {
  const bool cancellable = m_batchRenamer.applying()
      || m_archives.busy()
      || m_transfers.busy()
      || m_restore.busy()
      || m_trash.busy()
      || m_emptyTrash.busy();
  const bool retryable = m_transfers.retryable() > 0 && !m_page.operationsBusy();
  m_retryTransfer->setVisible(retryable);
  m_retryTransfer->setParticipatesInLayout(retryable);
  m_cancelOperation->setVisible(cancellable);
  m_cancelOperation->setParticipatesInLayout(cancellable);
  m_cancelOperation->setTooltip(
      m_batchRenamer.applying() ? "Cancel batch rename"
          : m_restore.busy()    ? "Cancel restoration"
          : m_emptyTrash.busy() ? (selectedTrashDelete ? "Cancel deletion" : "Cancel emptying Trash")
          : m_trash.busy()      ? "Cancel move to Trash"
                                : "Cancel transfer"
  );
}

void TransferController::stopProgress() { m_operationProgress.stop(); }

void TransferController::settle() {
  if (m_clipboardTransfer) {
    m_page.clipboard().finishMove(*m_clipboardTransfer, m_transfers.results());
  }
  if (m_dropCompletion) {
    m_dropCompletion(false);
  }
}
