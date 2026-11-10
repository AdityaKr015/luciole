#include "app/job_activity.h"

#include "app/operation_hub.h"
#include "fs/filename.h"
#include "fs/gio_ptr.h"

#include <algorithm>
#include <array>
#include <format>
#include <glib.h>
#include <string_view>

namespace {
  using files::ActivityState;
  using files::JobActivity;
  constexpr std::size_t kReportBytes = 64 * 1024;
  std::string bounded(std::string_view text) {
    constexpr std::string_view suffix = "\nReport truncated.\n";
    const auto count = std::min(text.size(), kReportBytes);
    const files::GString valid(g_utf8_make_valid(text.data(), static_cast<gssize>(count)));
    std::string result(valid.get());
    if (result.size() > kReportBytes - suffix.size() || text.size() > count) {
      auto limit = std::min(result.size(), kReportBytes - suffix.size());
      while (limit && (static_cast<unsigned char>(result[limit]) & 0xc0U) == 0x80U) {
        --limit;
      }
      result.resize(limit);
      result += suffix;
    }
    return result;
  }
  std::string sizeText(std::uint64_t bytes) {
    const files::GString text(g_format_size_full(bytes, G_FORMAT_SIZE_IEC_UNITS));
    return text.get();
  }
  std::string itemName(const files::Location& location) { return location.displayName().value_or("Item"); }
  struct Report {
    std::string text;
    std::size_t rows = 0;
    void add(const std::string& name, const std::string& outcome, const std::optional<files::FileError>& error = {}) {
      if (rows++ >= 2048 || text.size() >= kReportBytes) {
        return;
      }
      text += name + "\n  " + outcome + (error ? ": " + error->message : "") + '\n';
    }
    void publish(JobActivity& activity, bool details) const {
      if (details) {
        activity.report = std::make_shared<const std::string>(bounded(
            activity.title + "\n" + activity.summary + "\n\n" + text + (rows > 2048 ? "\nItem list truncated.\n" : "")
        ));
      }
    }
  };
  struct Counts {
    std::size_t done = 0, failed = 0, cancelled = 0, skipped = 0;
    bool partial = false;
    void finish(JobActivity& activity) const {
      activity.finished = true;
      activity.state = failed || partial ? (done || partial ? ActivityState::Partial : ActivityState::Failed)
          : cancelled                    ? ActivityState::Cancelled
                                         : ActivityState::Succeeded;
      activity.summary = std::format("{} completed", done);
      if (failed) {
        activity.summary += std::format(" · {} failed", failed);
      }
      if (skipped) {
        activity.summary += std::format(" · {} skipped", skipped);
      }
      if (cancelled) {
        activity.summary += std::format(" · {} cancelled", cancelled);
      }
      if (partial) {
        activity.summary += " · Incomplete changes";
      }
    }
  };
  void progress(JobActivity& activity, std::size_t done, std::size_t total) {
    activity.state = ActivityState::Working;
    activity.summary = total ? std::format("{} of {} items", done, total) : std::format("{} items", done);
    if (total) {
      activity.progress = std::clamp(static_cast<float>(done) / static_cast<float>(total), 0.0F, 1.0F);
    }
  }
  void transferResults(JobActivity& activity, const std::vector<files::TransferResult>& results, bool details) {
    constexpr std::array names{"Moved", "Copied", "Skipped", "Failed", "Cancelled", "Partial", "Linked"};
    Counts counts;
    Report report;
    for (const auto& result : results) {
      switch (result.outcome) {
      case files::TransferOutcome::Moved:
      case files::TransferOutcome::Copied:
      case files::TransferOutcome::Linked:
        ++counts.done;
        break;
      case files::TransferOutcome::Skipped:
        ++counts.skipped;
        break;
      case files::TransferOutcome::Failed:
        ++counts.failed;
        break;
      case files::TransferOutcome::Cancelled:
        ++counts.cancelled;
        break;
      case files::TransferOutcome::Partial:
        counts.partial = true;
        break;
      }
      if (details) {
        const auto append = [&](auto&& self, const files::TransferResult& item, unsigned depth) -> void {
          if (report.rows >= 2048 || report.text.size() >= kReportBytes || depth > 128) {
            report.rows = 2049;
            return;
          }
          report.add(itemName(item.source), names.at(static_cast<std::size_t>(item.outcome)), item.error);
          for (const auto& child : item.children) {
            self(self, child, depth + 1);
          }
        };
        append(append, result, 0);
      }
    }
    counts.finish(activity);
    report.publish(activity, details);
  }
  void simple(
      JobActivity& activity, bool running, bool ready, bool succeeded, bool cancelled,
      const std::optional<files::FileError>& error, bool details, const std::string& name = ""
  ) {
    activity.finished = !running && !ready;
    activity.state = error && error->kind != files::FileErrorKind::Cancelled     ? ActivityState::Failed
        : cancelled || (error && error->kind == files::FileErrorKind::Cancelled) ? ActivityState::Cancelled
        : succeeded                                                              ? ActivityState::Succeeded
        : ready                                                                  ? ActivityState::Waiting
                                                                                 : ActivityState::Preparing;
    activity.summary = error ? error->message
        : cancelled          ? "Cancelled"
        : succeeded          ? "Completed"
        : ready              ? "Waiting for confirmation"
                             : "Preparing…";
    if (running && activity.state != ActivityState::Preparing) {
      activity.state = ActivityState::Working;
    }
    activity.canReview = ready;
    Report report;
    if (!name.empty()) {
      report.add(name, activity.summary);
    }
    report.publish(activity, details);
  }
  void transfer(const OperationJob& job, JobActivity& activity, bool details) {
    const auto& service = job.service<files::TransferService>();
    activity.title = service.action() == files::TransferAction::Copy ? "Copy files"
        : service.action() == files::TransferAction::Move            ? "Move files"
                                                                     : "Create links";
    if (job.running()) {
      activity.state = service.bytes() || service.completed() ? ActivityState::Working : ActivityState::Preparing;
      activity.summary = std::format(
          "{} transferred · {} of {} source items", sizeText(service.bytes()), service.completed(), service.total()
      );
    } else {
      transferResults(activity, service.results(), details);
      activity.canRetry = service.retryable() > 0;
      if (details && !service.report().empty()) {
        activity.report = std::make_shared<const std::string>(bounded(service.report()));
      }
    }
  }
  void archive(const OperationJob& job, JobActivity& activity, bool details) {
    const auto& service = job.service<files::ArchiveService>();
    activity.title = "Archive operation";
    if (job.running()) {
      switch (service.phase()) {
      case files::ArchivePhase::Reading:
        activity.summary = "Reading archive…";
        break;
      case files::ArchivePhase::Compressing:
        activity.summary = "Compressing…";
        break;
      case files::ArchivePhase::Extracting:
        activity.summary = "Extracting…";
        break;
      case files::ArchivePhase::Transferring:
        activity.summary = "Publishing output…";
        break;
      case files::ArchivePhase::Cleaning:
        activity.summary = "Cleaning up…";
        break;
      case files::ArchivePhase::WaitingPassword:
        activity.summary = "Waiting for a password";
        activity.canReview = true;
        break;
      case files::ArchivePhase::Idle:
        activity.summary = "Preparing…";
        break;
      }
      activity.summary += std::format(" · {} entries · {} processed", service.items(), sizeText(service.bytes()));
      activity.state = activity.canReview ? ActivityState::Waiting : ActivityState::Working;
    } else if (!service.results().empty()) {
      transferResults(activity, service.results(), details);
    } else {
      simple(activity, false, false, !service.error(), false, service.error(), details);
    }
    if (service.error()) {
      const bool hasResults = !service.results().empty();
      if (!hasResults || !activity.failed()) {
        activity.state =
            service.error()->kind == files::FileErrorKind::Cancelled ? ActivityState::Cancelled : ActivityState::Failed;
      }
      activity.summary = (hasResults ? activity.summary + " · " : "") + service.error()->message;
      if (details) {
        activity.report = std::make_shared<const std::string>(
            bounded((activity.report ? *activity.report + "\n" : "") + service.error()->message)
        );
      }
    }
  }
  void single(const OperationJob& job, JobActivity& activity, bool details) {
    if (job.kind() == JobKind::Rename) {
      const auto& service = job.service<files::RenameService>();
      const auto& snapshot = service.snapshot();
      activity.title = job.renameReplay() == RenameReplay::Undo ? "Undo rename"
          : job.renameReplay() == RenameReplay::Redo            ? "Redo rename"
                                                                : "Rename item";
      simple(
          activity, job.running(), service.canRename(), snapshot.status == files::RenameStatus::Succeeded,
          snapshot.status == files::RenameStatus::Cancelled, snapshot.error, details,
          snapshot.source ? itemName(*snapshot.source) : ""
      );
      if (snapshot.status == files::RenameStatus::Renaming) {
        activity.state = ActivityState::Working;
        activity.summary = "Renaming…";
      }
    } else {
      const auto& service = job.service<files::CreateService>();
      const auto& snapshot = service.snapshot();
      activity.title = snapshot.kind == files::CreateKind::Folder ? "Create folder"
          : snapshot.kind == files::CreateKind::EmptyFile         ? "Create file"
          : snapshot.kind == files::CreateKind::Document          ? "Create document"
                                                                  : "Create link";
      simple(
          activity, job.running(), service.canCreate(), snapshot.status == files::CreateStatus::Succeeded,
          snapshot.status == files::CreateStatus::Cancelled, snapshot.error, details,
          snapshot.destination ? itemName(*snapshot.destination) : ""
      );
      if (snapshot.status == files::CreateStatus::Creating) {
        activity.state = ActivityState::Working;
        activity.summary = "Creating…";
      }
      if (snapshot.status == files::CreateStatus::Partial) {
        activity.state = ActivityState::Partial;
      }
    }
  }
  void trash(const OperationJob& job, JobActivity& activity, bool details) {
    Counts counts;
    Report report;
    if (job.kind() == JobKind::Trash) {
      const auto& service = job.service<files::TrashService>();
      const auto& snapshot = service.snapshot();
      activity.title = "Move to Trash";
      simple(
          activity, job.running(), service.canTrash(), snapshot.status == files::TrashStatus::Finished,
          snapshot.status == files::TrashStatus::Cancelled, snapshot.error, false
      );
      if (job.running() && snapshot.status == files::TrashStatus::Trashing) {
        progress(activity, service.completed(), snapshot.results.size());
      }
      if (!job.running()) {
        for (const auto& result : snapshot.results) {
          if (result.outcome == files::TrashOutcome::Trashed) {
            ++counts.done;
          } else if (result.outcome == files::TrashOutcome::Failed) {
            ++counts.failed;
          } else {
            ++counts.cancelled;
          }
          if (details) {
            report.add(
                itemName(result.source), result.outcome == files::TrashOutcome::Trashed ? "Trashed" : "Not trashed",
                result.error
            );
          }
        }
      }
      if (snapshot.status == files::TrashStatus::Finished) {
        counts.finish(activity);
      }
    } else {
      const auto& service = job.service<files::EmptyTrashService>();
      const auto& snapshot = service.snapshot();
      activity.title = snapshot.selection ? "Delete from Trash" : "Empty Trash";
      simple(
          activity, job.running(), service.canEmpty(), snapshot.status == files::EmptyTrashStatus::Finished,
          snapshot.status == files::EmptyTrashStatus::Cancelled, snapshot.error, false
      );
      if (job.running() && snapshot.status == files::EmptyTrashStatus::Emptying) {
        progress(activity, service.completed(), snapshot.results.size());
      }
      if (!job.running()) {
        for (const auto& result : snapshot.results) {
          if (result.outcome == files::EmptyTrashOutcome::Deleted) {
            ++counts.done;
          } else if (result.outcome == files::EmptyTrashOutcome::Skipped) {
            ++counts.skipped;
          } else if (result.outcome == files::EmptyTrashOutcome::Cancelled) {
            ++counts.cancelled;
          } else {
            ++counts.failed;
            counts.partial |= result.outcome == files::EmptyTrashOutcome::Unconfirmed;
          }
          if (details) {
            report.add(
                result.displayName,
                result.outcome == files::EmptyTrashOutcome::Deleted           ? "Deleted"
                    : result.outcome == files::EmptyTrashOutcome::Unconfirmed ? "Deletion unconfirmed"
                                                                              : "Not deleted",
                result.error
            );
          }
        }
      }
      if (snapshot.status == files::EmptyTrashStatus::Finished) {
        counts.finish(activity);
      }
    }
    report.publish(activity, details);
  }
  void restore(const OperationJob& job, JobActivity& activity, bool details) {
    const auto& service = job.service<files::RestoreService>();
    activity.title = "Restore from Trash";
    if (job.running()) {
      activity.state = ActivityState::Working;
      progress(activity, service.completed(), service.total());
      return;
    }
    Counts counts;
    Report report;
    for (const auto& result : service.results()) {
      if (result.outcome == files::RestoreOutcome::Restored) {
        ++counts.done;
      } else if (result.outcome == files::RestoreOutcome::Skipped) {
        ++counts.skipped;
      } else if (result.outcome == files::RestoreOutcome::Cancelled) {
        ++counts.cancelled;
      } else {
        ++counts.failed;
        counts.partial |= result.outcome == files::RestoreOutcome::Unconfirmed;
      }
      if (details) {
        report.add(
            result.displayName,
            result.outcome == files::RestoreOutcome::Restored          ? "Restored"
                : result.outcome == files::RestoreOutcome::Unconfirmed ? "Restoration unconfirmed"
                                                                       : "Not restored",
            result.error
        );
      }
    }
    counts.finish(activity);
    report.publish(activity, details);
  }
  void editing(const OperationJob& job, JobActivity& activity, bool details) {
    activity.state = ActivityState::Working;
    if (job.kind() == JobKind::BatchRename) {
      const auto& service = job.service<files::BatchRenameService>();
      const auto& snapshot = service.snapshot();
      activity.title = "Batch rename";
      if (job.running()) {
        if (service.applying()) {
          progress(activity, service.completed(), snapshot.rows ? snapshot.rows->size() : 0);
        } else {
          activity.state = ActivityState::Preparing;
          activity.summary = "Checking rename preview…";
        }
      } else {
        simple(
            activity, false, snapshot.phase == files::BatchRenamePhase::Ready,
            snapshot.phase == files::BatchRenamePhase::Finished, snapshot.phase == files::BatchRenamePhase::Cancelled,
            snapshot.error, details
        );
        if (snapshot.results) {
          Counts counts;
          for (const auto& result : *snapshot.results) {
            if (result.outcome == files::BatchRenameOutcome::Renamed
                || result.outcome == files::BatchRenameOutcome::Unchanged) {
              ++counts.done;
            } else if (result.outcome == files::BatchRenameOutcome::Cancelled) {
              ++counts.cancelled;
            } else {
              ++counts.failed;
              counts.partial |= result.outcome != files::BatchRenameOutcome::Failed;
            }
          }
          counts.finish(activity);
        }
      }
      if (details && !snapshot.report.empty()) {
        activity.report = std::make_shared<const std::string>(bounded(snapshot.report));
      }
    } else {
      const auto& service = job.service<files::PermissionService>();
      const auto& snapshot = service.snapshot();
      activity.title = "Change permissions";
      activity.finished = !job.running();
      if (job.running()) {
        if (service.collecting()) {
          activity.summary = "Collecting targets…";
        } else {
          progress(activity, service.completed(), service.total());
        }
      } else {
        activity.state = snapshot.failed() ? (snapshot.applied ? ActivityState::Partial : ActivityState::Failed)
            : snapshot.cancelRequested     ? ActivityState::Cancelled
                                           : ActivityState::Succeeded;
        activity.summary = snapshot.message.empty() ? "Permissions updated" : snapshot.message;
        if (snapshot.error) {
          activity.summary = snapshot.error->message;
        }
      }
      if (details) {
        activity.report =
            std::make_shared<const std::string>(bounded(snapshot.report.empty() ? activity.summary : snapshot.report));
      }
    }
  }
} // namespace

std::optional<files::JobActivity> jobActivity(const OperationJob& job, bool details) {
  if (!job.ran() || (job.kind() == JobKind::Permission && !job.activityStarted())) {
    return {};
  }
  files::JobActivity activity;
  activity.id = job.id();
  activity.canCancel = job.cancellable();
  switch (job.kind()) {
  case JobKind::Transfer:
    transfer(job, activity, details);
    break;
  case JobKind::Archive:
    archive(job, activity, details);
    break;
  case JobKind::Rename:
  case JobKind::Create:
    single(job, activity, details);
    break;
  case JobKind::Trash:
  case JobKind::EmptyTrash:
    trash(job, activity, details);
    break;
  case JobKind::Restore:
    restore(job, activity, details);
    break;
  case JobKind::BatchRename:
  case JobKind::Permission:
    editing(job, activity, details);
    break;
  }
  if (job.conflict()) {
    activity.state = files::ActivityState::Waiting;
    activity.summary = "Waiting for a conflict decision";
    activity.canReview = true;
  }
  if (activity.finished && !job.activityStarted() && !activity.failed() && job.kind() != JobKind::Archive) {
    return {};
  }
  if (details && !activity.report) {
    Report{}.publish(activity, true);
  }
  activity.title = bounded(activity.title);
  activity.summary = bounded(activity.summary);
  return activity;
}
