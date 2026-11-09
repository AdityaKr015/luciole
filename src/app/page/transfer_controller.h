#pragma once

#include "browser/action_registry.h"
#include "clipboard/text_clipboard.h"
#include "core/timer_manager.h"
#include "fs/transfer_types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace files {
  class ArchiveService;
  class BatchRenameService;
  class EmptyTrashService;
  class PermissionService;
  class RestoreService;
  class TransferService;
  class TrashService;
} // namespace files

class Button;
class PageContext;

// Copying and moving files: clipboard paste, drops and transfers to another page, the conflict dialog shared with
// archive and restore jobs, retrying failed items, and the status bar progress, retry and cancel controls.
class TransferController {
public:
  // `progressTick` runs every 100 ms while any operation runs.
  TransferController(
      PageContext& page, files::TransferService& transfers, files::ArchiveService& archives,
      files::RestoreService& restore, files::TrashService& trash, files::EmptyTrashService& emptyTrash,
      files::BatchRenameService& batchRenamer, files::PermissionService& permissions, std::function<void()> progressTick
  );

  // Pastes the clipboard's files into the request's folder once the clipboard answers.
  void paste(const files::ActionRequest& request);
  // True from a paste request until the clipboard answers.
  bool pastePending() const { return m_pendingFilePaste; }
  void cancelPaste() { m_pendingFilePaste = false; }
  // Copies or moves the request's targets into `destination`.
  void transferTo(const files::ActionRequest& request, const files::Location& destination, bool copy);
  // Transfers a dropped payload; `finish` reports whether every item arrived.
  void receiveDrop(
      files::FilePayload payload, const files::Location& destination, files::TransferAction action,
      files::DropCompletion finish
  );
  // TransferService change: reports the outcome once no transfer or archive job runs.
  void changed();
  // Shows the pending restore, archive or transfer conflict, or closes the dialog once it is resolved.
  // Frame slot in BrowserPage::prepareFrame() and pumpJobs().
  void syncConflict();
  // Keeps the shown conflict pending when its dialog closes, for a page that is deactivated.
  void keepConflict() { m_activeConflict.reset(); }
  // Runs the progress tick while any operation runs. Every operation service change calls this.
  void updateOperationProgress();

  // Status bar controls, built in BrowserPage::buildScene() and synced in BrowserPage::syncBrowser().
  std::unique_ptr<Button> buildRetryButton();
  std::unique_ptr<Button> buildCancelButton();
  void syncOperationButtons(bool selectedTrashDelete);

  // Page teardown: stop the tick, then settle a claimed clipboard move and fail a pending drop.
  void stopProgress();
  void settle();

private:
  bool
  startPayloadTransfer(files::FilePayload payload, const files::Location& destination, files::TransferAction action);
  void retry();

  PageContext& m_page;
  files::TransferService& m_transfers;
  files::ArchiveService& m_archives;
  files::RestoreService& m_restore;
  files::TrashService& m_trash;
  files::EmptyTrashService& m_emptyTrash;
  files::BatchRenameService& m_batchRenamer;
  files::PermissionService& m_permissions;
  std::function<void()> m_progressTick;
  Timer m_operationProgress;
  Button* m_cancelOperation = nullptr;
  Button* m_retryTransfer = nullptr;
  bool m_pendingFilePaste = false;
  std::optional<files::ClipboardFiles> m_clipboardTransfer;
  files::DropCompletion m_dropCompletion;
  std::optional<std::uint64_t> m_activeConflict;
  bool m_conflictArchive = false;
  bool m_conflictRestore = false;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
