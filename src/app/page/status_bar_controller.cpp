#include "app/page/status_bar_controller.h"

#include "app/page/file_keyboard_controller.h"
#include "app/page/page_context.h"
#include "app/page/settings_controller.h"
#include "app/page/transfer_controller.h"
#include "app/page/trash_controller.h"
#include "app/workspace_layout.h"
#include "browser/browser_controller.h"
#include "browser/places.h"
#include "fs/gio_ptr.h"
#include "operations/archive_service.h"
#include "operations/batch_rename_service.h"
#include "operations/create_service.h"
#include "operations/empty_trash_service.h"
#include "operations/permission_service.h"
#include "operations/rename_service.h"
#include "operations/restore_service.h"
#include "operations/transfer_service.h"
#include "operations/trash_service.h"
#include "ui/builders.h"
#include "ui/palette.h"

#include <algorithm>
#include <format>
#include <utility>

StatusBarController::StatusBarController(
    PageContext& page, files::CreateService& creator, files::PermissionService& permissions,
    files::BatchRenameService& batchRenamer, files::RenameService& renamer, files::RestoreService& restore,
    files::EmptyTrashService& emptyTrash, files::TrashService& trash, files::ArchiveService& archives,
    files::TransferService& transfers, files::PinService& pins, TrashController& trashController,
    TransferController& transferController, SettingsController& settings, FileKeyboardController& keys
)
    : m_page(page), m_creator(creator), m_permissions(permissions), m_batchRenamer(batchRenamer), m_renamer(renamer),
      m_restore(restore), m_emptyTrash(emptyTrash), m_trash(trash), m_archives(archives), m_transfers(transfers),
      m_pins(pins), m_trashController(trashController), m_transferController(transferController), m_settings(settings),
      m_keys(keys) {}

std::unique_ptr<Flex> StatusBarController::build() {
  return ui::row(
      {.align = FlexAlign::Center,
       .gap = Style::spaceMd,
       .paddingV = Style::spaceSm,
       .paddingH = Style::spaceSm + Style::spaceXs},
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
      m_transferController.buildRetryButton(), m_transferController.buildCancelButton()
  );
}

std::unique_ptr<Flex> StatusBarController::buildNotice() {
  return ui::column(
      {.out = &m_noticeArea, .align = FlexAlign::Stretch, .flexGrow = 1.0F},
      ui::label({.out = &m_notice, .textAlign = TextAlign::Center})
  );
}

std::string StatusBarController::operationStatus() const {
  std::string status;
  if (m_creator.busy()) {
    const char* kind = m_creator.snapshot().kind == files::CreateKind::Folder ? "folder"
        : m_creator.snapshot().kind == files::CreateKind::Document            ? "document"
        : m_creator.snapshot().kind == files::CreateKind::SymbolicLink
            || m_creator.snapshot().kind == files::CreateKind::HardLink
        ? "link"
        : "file";
    status = m_creator.snapshot().status == files::CreateStatus::Creating ? std::format("Creating {}…", kind)
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
              "{}… · {} of {} items", m_trashController.selectedDelete() ? "Deleting from Trash" : "Emptying Trash",
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
  }
  return status;
}

void StatusBarController::sync(bool miller) {
  const auto& model = m_page.browser().model();
  const auto& snapshot = model.snapshot();
  const bool loading = snapshot.status == files::DirectoryStatus::Loading;
  std::string status = operationStatus();
  if (status.empty()) {
    if (!m_actionError.empty()) {
      status = m_actionError;
    } else if (!m_pins.error().empty()) {
      status = std::format("Could not update pins: {}", m_pins.error());
    } else if (!m_settings.error().empty()) {
      status = m_settings.error();
    } else if (!m_page.browser().message().empty()) {
      status = m_page.browser().message();
    } else if (snapshot.error) {
      status = snapshot.error->message;
    } else if (!m_page.browser().searchQuery().empty()) {
      status = std::format(
          "{}{} results",
          loading                                                    ? "Searching… · "
              : snapshot.status == files::DirectoryStatus::Cancelled ? "Search stopped · "
                                                                     : "",
          model.rows().size()
      );
      if (m_page.browser().skippedSearchFolders() != 0) {
        status += std::format(" · {} folders could not be searched", m_page.browser().skippedSearchFolders());
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
  if (const auto typed = m_keys.typeaheadText(); !typed.empty()) {
    status = "Select: " + typed + " · " + status;
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
  m_transferController.syncOperationButtons(m_trashController.selectedDelete());
  m_notice->setText(
      snapshot.error
              || loading
              || snapshot.status == files::DirectoryStatus::Cancelled
              || !m_page.browser().message().empty()
              || !m_settings.error().empty()
          ? status
          : !m_page.browser().searchQuery().empty() ? "No matching files or folders."
          : snapshot.entries.empty()
          ? (snapshot.location == files::Location::trash() ? "Trash is empty." : "This folder is empty.")
          : "No visible items."
  );
  const bool empty = model.rows().empty();
  m_noticeArea->setVisible(empty && !miller);
  m_noticeArea->setParticipatesInLayout(empty && !miller);
}

void StatusBarController::layout(float paneWidth) {
  const bool showFreeSpace = m_freeSpaceAvailable && paneWidth >= kMinimumFreeSpacePaneWidth;
  m_freeSpace->setVisible(showFreeSpace);
  m_freeSpace->setParticipatesInLayout(showFreeSpace);
}

bool StatusBarController::noticeShown() const { return m_notice != nullptr && m_noticeArea->visible(); }

LayoutRect StatusBarController::statusRect() const { return m_status ? sceneRect(*m_status) : LayoutRect{}; }
