#include "operations/transfer_service.h"

#include "fs/background_job.h"
#include "fs/cancel_token.h"
#include "fs/filename.h"
#include "fs/posix_fd.h"
#include "operations/conflict_gate.h"
#include "operations/transfer_copy.h"
#include "operations/transfer_staging.h"

#include <array>
#include <atomic>
#include <fcntl.h>
#include <filesystem>
#include <glib.h>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace {
  using namespace files::transfer;

  struct Job {
    explicit Job(files::TransferRequest transfer) : request(std::move(transfer)) {}
    files::TransferRequest request;
    std::vector<std::shared_ptr<Node>> nodes;
    std::map<std::pair<dev_t, ino_t>, struct stat> changedLinks;
    std::size_t mergedItems = 0;
    std::vector<files::TransferResult> outcomes;
  };

  struct Answer {
    files::TransferConflictChoice choice;
    bool applyToAll = false;
  };
  using Gate = files::ConflictGate<files::TransferConflict, Answer>;

  struct Reply {
    std::vector<files::TransferResult> results;
    std::string report;
  };

  std::string jobReport(const std::vector<files::TransferResult>& results) {
    std::string report = "Transfer results\n";
    constexpr std::array labels{"Moved", "Copied", "Skipped", "Failed", "Cancelled", "Partial"};
    std::function<void(const files::TransferResult&)> append = [&](const auto& result) {
      if (report.size() > 16 * 1024 * 1024) {
        return;
      }
      report += result.source.uri()
          + (result.destination ? " -> " + result.destination->uri() : "")
          + "\n  "
          + labels.at(static_cast<std::size_t>(result.outcome));
      report += result.retryable ? " (retry available)" : "";
      report += result.error ? "\n  " + result.error->message + "\n" : "\n";
      for (const auto& child : result.children) {
        append(child);
      }
    };
    for (const auto& result : results) {
      append(result);
    }
    if (report.size() > 16 * 1024 * 1024) {
      report += "\nReport truncated.\n";
    }
    return report;
  }

  files::FileError cancelledError() {
    return {.kind = files::FileErrorKind::Cancelled, .message = "Transfer cancelled."};
  }
} // namespace

struct files::TransferService::State {
  std::function<void()> changed;
  bool active = false;
  TransferAction action = TransferAction::Move;
  std::size_t total = 0;
  std::atomic<std::size_t> completed{0};
  std::atomic<std::uint64_t> bytes{0};
  CancelToken token;
  std::vector<TransferResult> results;
  Gate gate;
  std::uint64_t jobId = 0;
  std::shared_ptr<Job> job;
  std::size_t retryable = 0;
  std::string report;

  static void notify(const std::shared_ptr<State>& owner) {
    auto* source = g_idle_source_new();
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(
        source,
        [](gpointer data) -> gboolean {
          const auto& retained = *static_cast<std::shared_ptr<State>*>(data);
          const auto callback = retained->changed;
          if (callback) {
            callback();
          }
          return G_SOURCE_REMOVE;
        },
        new std::shared_ptr<State>(owner), [](gpointer data) { delete static_cast<std::shared_ptr<State>*>(data); }
    );
    g_source_attach(source, nullptr);
    g_source_unref(source);
  }
};

namespace {
  // Worker side of a conflict: remembered policies first, then the gate.
  Work::Choose conflictAsker(
      Gate& gate, std::function<void()> published, files::CancelToken token, bool allowMerge,
      std::map<std::pair<mode_t, mode_t>, files::TransferConflictChoice>& policies
  ) {
    return [&gate, published = std::move(published), token = std::move(token), allowMerge, &policies](
               const Source& source, const files::Location& destination, const std::string& target,
               const struct stat& existing, bool forcePrompt
           ) {
      const auto kind = std::pair{source.metadata.st_mode & S_IFMT, existing.st_mode & S_IFMT};
      if (!forcePrompt) {
        if (const auto found = policies.find(kind); found != policies.end()) {
          return found->second;
        }
      }
      const bool folders = S_ISDIR(source.metadata.st_mode) && S_ISDIR(existing.st_mode);
      files::TransferConflict conflict{
          .id = 0,
          .source = source.location,
          .destination = destination.child(target),
          .name = files::filenameDisplayText(target),
          .sourceDirectory = S_ISDIR(source.metadata.st_mode),
          .destinationDirectory = S_ISDIR(existing.st_mode),
          .sourceSize = S_ISDIR(source.metadata.st_mode) || source.metadata.st_size < 0
              ? std::nullopt
              : std::optional<std::uint64_t>(source.metadata.st_size),
          .destinationSize = S_ISDIR(existing.st_mode) || existing.st_size < 0
              ? std::nullopt
              : std::optional<std::uint64_t>(existing.st_size),
          .sourceModified = source.metadata.st_mtim.tv_sec < 0
              ? std::nullopt
              : std::optional<std::uint64_t>(source.metadata.st_mtim.tv_sec),
          .destinationModified =
              existing.st_mtim.tv_sec < 0 ? std::nullopt : std::optional<std::uint64_t>(existing.st_mtim.tv_sec),
          .canMerge = allowMerge && folders,
          .canApplyToAll = allowMerge,
          .scopeLabel = folders ? "Apply to remaining folder conflicts in this job"
                                : "Apply to remaining conflicts of these types in this job"
      };
      const auto answer = gate.ask(std::move(conflict), token, published);
      if (!answer) {
        return files::TransferConflictChoice::Cancel;
      }
      if (answer->applyToAll && answer->choice != files::TransferConflictChoice::Cancel) {
        policies.insert_or_assign(kind, answer->choice);
      }
      return answer->choice;
    };
  }

  std::vector<files::TransferResult> retryJob(Job& job, Work& work, std::atomic<std::size_t>& completed) {
    auto outcomes = job.outcomes;
    for (const auto& node : job.nodes) {
      if (node) {
        execute(*node, job.request.action, work, true);
        outcomes.at(node->source.index) = snapshot(*node);
      }
      completed.fetch_add(1);
    }
    return outcomes;
  }

  std::vector<files::TransferResult> runFirst(Job& job, Work& work, std::atomic<std::size_t>& completed) {
    using files::TransferOutcome;
    const auto& request = job.request;
    std::vector<files::TransferResult> outcomes;
    outcomes.reserve(request.sources.size());
    for (const auto& source : request.sources) {
      outcomes.emplace_back(source, TransferOutcome::Failed);
    }
    std::vector<bool> processed(outcomes.size(), false);
    job.nodes.resize(outcomes.size());
    try {
      const auto path = request.destination.nativePath();
      if (!path) {
        files::throwFileError(files::FileErrorKind::Unsupported, "Transfers to remote folders are not supported yet.");
      }
      auto destination = opened(::open(path->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      auto binding =
          std::make_shared<const Destination>(Destination{request.destination, files::statFd(destination.get()), {}});
      std::vector<Source> sources;
      for (std::size_t index = 0; index < request.sources.size(); ++index) {
        const auto& location = request.sources[index];
        try {
          work.check();
          const auto sourcePath = location.nativePath();
          if (!sourcePath) {
            files::throwFileError(
                files::FileErrorKind::Unsupported, "Transfers from remote locations are not supported yet."
            );
          }
          const std::filesystem::path source(*sourcePath);
          const auto name = source.filename().native();
          if (name.empty() || name == "." || name == "..") {
            files::throwFileError(files::FileErrorKind::InvalidArgument, "The filesystem root cannot be transferred.");
          }
          auto parent = opened(::open(source.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
          const auto metadata = info(parent.get(), name);
          sources.push_back(
              {index, location, files::Fd{}, name, metadata, source.parent_path().native(), files::statFd(parent.get())}
          );
        } catch (const TransferCancelled&) {
          outcomes[index] = {location, TransferOutcome::Cancelled, cancelledError()};
          processed[index] = true;
          completed.fetch_add(1);
        } catch (const std::exception&) {
          outcomes[index] = {location, TransferOutcome::Failed, files::currentFileError()};
          processed[index] = true;
          completed.fetch_add(1);
        }
      }
      std::set<std::pair<dev_t, ino_t>> selectedDirectories;
      for (const auto& source : sources) {
        if (S_ISDIR(source.metadata.st_mode)) {
          selectedDirectories.emplace(source.metadata.st_dev, source.metadata.st_ino);
        }
      }
      std::set<std::tuple<dev_t, ino_t, std::string>> seen;
      std::vector<bool> included(sources.size(), false);
      // Classify overlaps before any selected parent is moved or removed.
      for (std::size_t i = 0; i < sources.size(); ++i) {
        auto& source = sources[i];
        try {
          work.check();
          source.parent = openSourceParent(source);
          const auto parentInfo = source.parentMetadata;
          included[i] = !seen.emplace(parentInfo.st_dev, parentInfo.st_ino, source.name).second;
          auto current = opened(::fcntl(source.parent.get(), F_DUPFD_CLOEXEC, 0));
          for (std::size_t depth = 0; !included[i] && !selectedDirectories.empty(); ++depth) {
            work.check();
            if (depth >= 65536) {
              files::throwFileError(files::FileErrorKind::Unsupported, "The folder ancestry is too deep.");
            }
            const auto here = files::statFd(current.get());
            if (selectedDirectories.contains({here.st_dev, here.st_ino})) {
              included[i] = true;
              break;
            }
            auto parent = directory(current.get(), "..");
            if (files::sameIdentity(here, files::statFd(parent.get()))) {
              break;
            }
            current = std::move(parent);
          }
        } catch (const TransferCancelled&) {
          outcomes[source.index] = {source.location, TransferOutcome::Cancelled, cancelledError()};
          processed[source.index] = true;
          completed.fetch_add(1);
        } catch (const std::exception&) {
          outcomes[source.index] = {source.location, TransferOutcome::Failed, files::currentFileError()};
          processed[source.index] = true;
          completed.fetch_add(1);
        }
        source.parent = files::Fd{};
      }

      for (std::size_t i = 0; i < sources.size(); ++i) {
        auto& source = sources[i];
        if (processed[source.index]) {
          continue;
        }
        try {
          if (included[i]) {
            outcomes[source.index].outcome = TransferOutcome::Skipped;
          } else {
            Source captured{source.index,    source.location,   files::Fd{},          source.name,
                            source.metadata, source.parentPath, source.parentMetadata};
            auto node = std::make_shared<Node>(std::move(captured), binding, outcomes[source.index]);
            job.nodes[source.index] = node;
            execute(*node, request.action, work, false);
            outcomes[source.index] = snapshot(*node);
          }
        } catch (const std::exception&) {
          outcomes[source.index].error = files::currentFileError();
        }
        source.parent = files::Fd{};
        processed[source.index] = true;
        completed.fetch_add(1);
      }
    } catch (const std::exception&) {
      const auto error = files::currentFileError();
      for (std::size_t i = 0; i < outcomes.size(); ++i) {
        if (!processed[i]) {
          outcomes[i].error = error;
        }
      }
    }
    completed.store(outcomes.size());
    return outcomes;
  }
} // namespace

files::TransferService::TransferService() : m_state(std::make_shared<State>()) {}
files::TransferService::~TransferService() {
  m_state->changed = {};
  cancel();
}
void files::TransferService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
bool files::TransferService::busy() const { return m_state->active; }
std::size_t files::TransferService::total() const { return m_state->total; }
std::size_t files::TransferService::completed() const { return m_state->completed.load(); }
std::uint64_t files::TransferService::bytes() const { return m_state->bytes.load(); }
files::TransferAction files::TransferService::action() const { return m_state->action; }
const std::vector<files::TransferResult>& files::TransferService::results() const { return m_state->results; }
void files::TransferService::cancel() {
  m_state->token.cancel();
  m_state->gate.wake();
}
std::optional<files::TransferConflict> files::TransferService::conflict() const { return m_state->gate.pending(); }
bool files::TransferService::resolve(std::uint64_t id, TransferConflictChoice choice, bool applyToAll) {
  if (choice < TransferConflictChoice::Replace || choice > TransferConflictChoice::Merge) {
    return false;
  }
  const auto& token = m_state->token;
  return m_state->gate.resolve(id, Answer{.choice = choice, .applyToAll = applyToAll}, [&](const auto& conflict) {
    if ((choice == TransferConflictChoice::Merge && !conflict.canMerge)
        || (applyToAll && (!conflict.canApplyToAll || choice == TransferConflictChoice::Cancel))) {
      return false;
    }
    if (choice == TransferConflictChoice::Cancel) {
      token.cancel();
    }
    return true;
  });
}

std::size_t files::TransferService::retryable() const { return busy() ? 0 : m_state->retryable; }
const std::string& files::TransferService::report() const { return m_state->report; }
std::uint64_t files::TransferService::jobId() const { return m_state->jobId; }
bool files::TransferService::start(TransferRequest request) { return launch(std::move(request), false); }
bool files::TransferService::retryFailed() {
  return m_state->job && retryable() > 0 ? launch(m_state->job->request, true) : false;
}
bool files::TransferService::launch(TransferRequest request, bool retry) {
  if (busy() || request.sources.empty()) {
    return false;
  }
  m_state->active = true;
  m_state->action = request.action;
  m_state->total = request.sources.size();
  m_state->completed.store(0);
  m_state->bytes.store(0);
  m_state->token = CancelToken{};
  if (!retry) {
    ++m_state->jobId;
    m_state->results.clear();
    m_state->report.clear();
    m_state->retryable = 0;
    m_state->job.reset();
  }
  auto job = retry ? m_state->job : std::make_shared<Job>(std::move(request));
  runJob<Reply>(
      [owner = m_state, job, token = m_state->token, retry] {
        std::map<std::pair<mode_t, mode_t>, TransferConflictChoice> policies;
        const bool allowMerge = job->request.allowMerge;
        Work work{
            token, owner->bytes,
            conflictAsker(owner->gate, [owner] { State::notify(owner); }, token, allowMerge, policies)
        };
        work.allowMerge = allowMerge;
        work.changedLinks = job->changedLinks;
        work.mergedItems = job->mergedItems;
        auto outcomes = retry ? retryJob(*job, work, owner->completed) : runFirst(*job, work, owner->completed);
        job->changedLinks = std::move(work.changedLinks);
        job->mergedItems = work.mergedItems;
        job->outcomes = outcomes;
        auto report = jobReport(outcomes);
        return Reply{.results = std::move(outcomes), .report = std::move(report)};
      },
      [owner = m_state, job](JobResult<Reply> result) {
        if (result) {
          owner->results = std::move(result->results);
          owner->report = std::move(result->report);
        } else {
          owner->results.clear();
          for (const auto& source : job->request.sources) {
            owner->results.emplace_back(source, TransferOutcome::Failed, result.error());
          }
          owner->report = jobReport(owner->results);
        }
        owner->job = job;
        owner->retryable = 0;
        for (const auto& node : job->nodes) {
          if (node) {
            owner->retryable += retryCount(*node);
          }
        }
        if (owner->retryable == 0 || !result) {
          owner->retryable = 0;
          owner->job.reset();
        }
        owner->active = false;
        const auto callback = owner->changed;
        if (callback) {
          callback();
        }
      }
  );
  const auto callback = m_state->changed;
  if (callback) {
    callback();
  }
  return true;
}
