#pragma once

#include "browser/action_registry.h"
#include "clipboard/text_clipboard.h"
#include "core/timer_manager.h"
#include "fs/transfer_types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace files {
  struct TransferRequest;
} // namespace files

class Button;
class OperationJob;
class PageContext;

// Copying and moving files: clipboard paste, drops and transfers to another page, the conflict dialog shared with
// archive and restore jobs, retrying failed items, and the status bar progress, retry and cancel controls. It works
// on the jobs the page owns, including the ones it took over from a closed page.
class TransferController {
public:
  // `progressTick` runs every 100 ms while any owned operation runs.
  TransferController(PageContext& page, std::function<void()> progressTick);

  // Pastes the clipboard's files into the request's folder once the clipboard answers.
  void paste(const files::ActionRequest& request);
  // True from a paste request until the clipboard answers.
  bool pastePending() const { return m_pendingFilePaste; }
  void cancelPaste() { m_pendingFilePaste = false; }
  // Copies or moves the request's targets into `destination`.
  void transferTo(const files::ActionRequest& request, const files::Location& destination, bool copy);
  // Starts a transfer job that copies or moves `sources` into `destination`.
  void transfer(std::vector<files::Location> sources, const files::Location& destination, files::TransferAction action);
  // Transfers a dropped payload; `finish` reports whether every item arrived.
  void receiveDrop(
      files::FilePayload payload, const files::Location& destination, files::TransferAction action,
      files::DropCompletion finish
  );
  // Transfer job change: reports the outcome once the job finishes.
  void transferChanged(OperationJob& job);
  // Shows the oldest pending restore, archive or transfer conflict, or closes the dialog once it is resolved.
  // Frame slot in BrowserPage::prepareFrame() and pumpJobs().
  void syncConflict();
  // Keeps the shown conflict pending when its dialog closes, for a page that is deactivated.
  void keepConflict() { m_activeConflict.reset(); }
  // Runs the progress tick while any owned operation runs. Every owned job change and every tick call this.
  void updateOperationProgress();
  // Report of the newest finished transfer the page owns; empty while it runs.
  std::string_view report() const;

  // Status bar controls, built in BrowserPage::buildScene() and synced in BrowserPage::syncBrowser().
  std::unique_ptr<Button> buildRetryButton();
  std::unique_ptr<Button> buildCancelButton();
  void syncOperationButtons();

  // Page teardown: stop the tick.
  void stopProgress();

private:
  struct ActiveConflict {
    std::uint64_t job = 0;
    std::uint64_t conflict = 0;
  };

  std::shared_ptr<OperationJob> start(files::TransferRequest request);
  std::shared_ptr<OperationJob>
  startPayloadTransfer(files::FilePayload payload, const files::Location& destination, files::TransferAction action);
  void finishMove(OperationJob& job, files::ClipboardFiles selection);
  OperationJob* latestTransfer() const;
  OperationJob* cancelTarget() const;
  void retry();

  PageContext& m_page;
  std::function<void()> m_progressTick;
  Timer m_operationProgress;
  Button* m_cancelOperation = nullptr;
  Button* m_retryTransfer = nullptr;
  bool m_pendingFilePaste = false;
  std::optional<ActiveConflict> m_activeConflict;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
