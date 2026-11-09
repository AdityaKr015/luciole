#include "app/page/status_bar_controller.h"

#include "app/operation_hub.h"
#include "app/page/file_keyboard_controller.h"
#include "app/page/page_context.h"
#include "app/page/settings_controller.h"
#include "app/page/transfer_controller.h"
#include "app/workspace_layout.h"
#include "browser/browser_controller.h"
#include "browser/places.h"
#include "fs/gio_ptr.h"
#include "ui/builders.h"
#include "ui/palette.h"

#include <algorithm>
#include <format>
#include <utility>

StatusBarController::StatusBarController(
    PageContext& page, files::PinService& pins, TransferController& transferController, SettingsController& settings,
    FileKeyboardController& keys
)
    : m_page(page), m_pins(pins), m_transferController(transferController), m_settings(settings), m_keys(keys) {}

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
  const auto* job = m_page.jobs().newest([](const OperationJob& candidate) { return candidate.running(); });
  if (!job) {
    return {};
  }
  switch (job->kind()) {
  case JobKind::Create: {
    const auto& snapshot = job->service<files::CreateService>().snapshot();
    const char* kind = snapshot.kind == files::CreateKind::Folder                                          ? "folder"
        : snapshot.kind == files::CreateKind::Document                                                     ? "document"
        : snapshot.kind == files::CreateKind::SymbolicLink || snapshot.kind == files::CreateKind::HardLink ? "link"
                                                                                                           : "file";
    return snapshot.status == files::CreateStatus::Creating ? std::format("Creating {}…", kind)
                                                            : std::format("Preparing {} creation…", kind);
  }
  case JobKind::Permission:
    return job->service<files::PermissionService>().snapshot().phase == files::PermissionPhase::Saving
        ? "Saving permissions…"
        : "Reading permissions…";
  case JobKind::BatchRename: {
    const auto& renamer = job->service<files::BatchRenameService>();
    return renamer.applying() ? std::format("Renaming… · {} items", renamer.completed()) : "Checking rename preview…";
  }
  case JobKind::Rename:
    return job->service<files::RenameService>().snapshot().status == files::RenameStatus::Renaming
        ? "Renaming…"
        : "Preparing rename…";
  case JobKind::Restore: {
    const auto& restore = job->service<files::RestoreService>();
    return std::format("Restoring… · {} of {} items", restore.completed(), restore.total());
  }
  case JobKind::EmptyTrash: {
    const auto& emptyTrash = job->service<files::EmptyTrashService>();
    const auto& snapshot = emptyTrash.snapshot();
    return snapshot.status == files::EmptyTrashStatus::Preparing ? "Reading Trash…"
        : snapshot.status == files::EmptyTrashStatus::Ready
        ? "Calculating Trash size…"
        : std::format(
              "{}… · {} of {} items", snapshot.selection ? "Deleting from Trash" : "Emptying Trash",
              emptyTrash.completed(), snapshot.results.size()
          );
  }
  case JobKind::Trash: {
    const auto& trash = job->service<files::TrashService>();
    return trash.snapshot().status == files::TrashStatus::Preparing
        ? "Preparing move to Trash…"
        : std::format("Moving to Trash… · {} of {} items", trash.completed(), trash.snapshot().results.size());
  }
  case JobKind::Archive: {
    const auto& archives = job->service<files::ArchiveService>();
    const files::GString size(g_format_size_full(archives.bytes(), G_FORMAT_SIZE_IEC_UNITS));
    const char* phase = archives.phase() == files::ArchivePhase::Reading ? "Reading archive"
        : archives.phase() == files::ArchivePhase::WaitingPassword       ? "Waiting for archive password"
        : archives.phase() == files::ArchivePhase::Compressing           ? "Compressing"
        : archives.phase() == files::ArchivePhase::Cleaning              ? "Cleaning temporary files"
        : archives.phase() == files::ArchivePhase::Transferring          ? "Publishing archive output"
                                                                         : "Extracting archive";
    return std::format("{}… · {} entries · {}", phase, archives.items(), size.get());
  }
  case JobKind::Transfer: {
    const auto& transfers = job->service<files::TransferService>();
    const files::GString size(g_format_size(transfers.bytes()));
    return std::format(
        "{} {} of {} items · {}", transfers.action() == files::TransferAction::Copy ? "Copying" : "Moving",
        transfers.completed(), transfers.total(), size.get()
    );
  }
  }
  return {};
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
  m_transferController.syncOperationButtons();
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
