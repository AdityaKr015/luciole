#include "operations/archive_service.h"

#include "fs/gio_ptr.h"
#include "operations/archive_work.h"

#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <utility>

struct files::ArchiveService::State : std::enable_shared_from_this<State> {
  enum class Operation { Inspect, Compress, Extract, Open };
  struct Request {
    std::shared_ptr<State> owner;
    Operation operation;
    std::vector<Location> sources;
    std::optional<Location> destination;
    ArchiveFormat format = ArchiveFormat::Zip;
    std::vector<std::string> selected;
    std::shared_ptr<const ArchiveCatalog> expected;
  };
  struct Result {
    archive_work::Prepared prepared;
    std::string error;
  };
  ArchiveLimits limits;
  archive_work::Progress progress;
  ArchivePhase phase = ArchivePhase::Idle;
  std::function<void()> changed;
  GioPtr<GCancellable> cancel{g_cancellable_new()};
  std::shared_ptr<const ArchiveCatalog> catalog;
  std::optional<Location> opened;
  std::vector<std::string> openStages;
  std::uint64_t openBytes = 0;
  std::string stage, error;
  bool closing = false;
  GSource* timer = nullptr;
  TransferService transfers;
  std::shared_ptr<State> transferLifetime;
  std::vector<TransferResult> results;
  explicit State(ArchiveLimits value) : limits(value) {}
  ~State() {
    if (timer) {
      g_source_destroy(timer);
      g_source_unref(timer);
    }
  }
  void notify() {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  void stopTimer() {
    if (timer) {
      g_source_destroy(timer);
      g_source_unref(std::exchange(timer, nullptr));
    }
  }
  void startTimer() {
    stopTimer();
    timer = g_timeout_source_new(100);
    g_source_set_callback(
        timer,
        [](gpointer data) -> gboolean {
          static_cast<State*>(data)->notify();
          return G_SOURCE_CONTINUE;
        },
        this, nullptr
    );
    g_source_attach(timer, g_main_context_default());
  }
  void clean(std::vector<std::string> paths) {
    stopTimer();
    if (closing) {
      for (auto& path : openStages) {
        paths.push_back(std::move(path));
      }
      openStages.clear();
      openBytes = 0;
    }
    if (paths.empty()) {
      phase = ArchivePhase::Idle;
      notify();
      return;
    }
    phase = ArchivePhase::Cleaning;
    struct Cleanup {
      std::shared_ptr<State> owner;
      std::vector<std::string> paths;
    };
    const GioPtr<GTask> task(g_task_new(
        nullptr, nullptr,
        [](GObject*, GAsyncResult* result, gpointer) {
          const auto owner = static_cast<Cleanup*>(g_task_get_task_data(G_TASK(result)))->owner;
          const GString problem(static_cast<char*>(g_task_propagate_pointer(G_TASK(result), nullptr)));
          if (problem && *problem) {
            owner->error = problem.get();
          }
          if (owner->closing && !owner->openStages.empty()) {
            owner->clean({});
          } else {
            owner->phase = ArchivePhase::Idle;
            owner->notify();
          }
        },
        nullptr
    ));
    g_task_set_task_data(task.get(), new Cleanup{shared_from_this(), std::move(paths)}, [](gpointer data) {
      delete static_cast<Cleanup*>(data);
    });
    g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable*) {
      const auto& cleanup = *static_cast<Cleanup*>(data);
      const auto error = archive_work::cleanup(cleanup.paths);
      g_task_return_pointer(task, g_strdup(error.c_str()), g_free);
    });
    notify();
  }
  void transferChanged() {
    if (transfers.busy()) {
      notify();
      return;
    }
    results = transfers.results();
    for (const auto& result : results) {
      if (!result.error.empty()) {
        error = result.error;
        break;
      }
      if (result.outcome == TransferOutcome::Cancelled) {
        error = "Archive operation cancelled.";
      } else if (result.outcome == TransferOutcome::Skipped) {
        error = "Archive operation skipped.";
      }
    }
    std::vector<std::string> paths;
    if (!stage.empty()) {
      paths.push_back(std::exchange(stage, {}));
    }
    clean(std::move(paths));
    transferLifetime.reset();
  }
  bool start(
      Operation operation, std::vector<Location> sources, std::optional<Location> destination, ArchiveFormat format,
      std::vector<std::string> selected
  ) {
    if (phase != ArchivePhase::Idle || closing || sources.empty()) {
      return false;
    }
    if (operation == Operation::Open && (openStages.size() >= 32 || openBytes >= 512U * 1024U * 1024U)) {
      error = "Temporary archive preview storage is full. Extract the member instead.";
      notify();
      return false;
    }
    error.clear();
    opened.reset();
    results.clear();
    progress.bytes.store(0);
    progress.items.store(0);
    cancel.reset(g_cancellable_new());
    auto expected = (catalog && catalog->source == sources.front()) ? catalog : nullptr;
    if (operation == Operation::Inspect) {
      catalog.reset();
      phase = ArchivePhase::Reading;
    } else {
      phase = operation == Operation::Compress ? ArchivePhase::Compressing : ArchivePhase::Extracting;
    }
    const GioPtr<GTask> task(g_task_new(
        nullptr, cancel.get(),
        [](GObject*, GAsyncResult* result, gpointer) {
          auto* task = G_TASK(result);
          const auto* request = static_cast<Request*>(g_task_get_task_data(task));
          const auto owner = request->owner;
          const std::unique_ptr<Result> outcome(static_cast<Result*>(g_task_propagate_pointer(task, nullptr)));
          owner->stopTimer();
          owner->error = outcome->error;
          if (owner->closing || g_cancellable_is_cancelled(g_task_get_cancellable(task))) {
            if (owner->error.empty()) {
              owner->error = "Archive operation cancelled.";
            }
            std::vector<std::string> paths;
            if (!outcome->prepared.stage.empty()) {
              paths.push_back(std::move(outcome->prepared.stage));
            }
            owner->clean(std::move(paths));
            return;
          }
          if (!owner->error.empty()) {
            owner->clean({});
            return;
          }
          if (request->operation == Operation::Inspect) {
            owner->catalog = std::move(outcome->prepared.catalog);
            owner->phase = ArchivePhase::Idle;
            owner->notify();
            return;
          }
          if (request->operation == Operation::Open) {
            if (outcome->prepared.expandedBytes > 512U * 1024U * 1024U - owner->openBytes) {
              owner->error = "Temporary archive preview storage is full. Extract the member instead.";
              owner->clean({outcome->prepared.stage});
              return;
            }
            owner->openStages.push_back(outcome->prepared.stage);
            owner->openBytes += outcome->prepared.expandedBytes;
            owner->opened = outcome->prepared.output;
            owner->phase = ArchivePhase::Idle;
            owner->notify();
            return;
          }
          owner->stage = std::move(outcome->prepared.stage);
          const std::filesystem::path target(*request->destination->nativePath());
          owner->phase = ArchivePhase::Transferring;
          owner->transferLifetime = owner;
          if (!owner->transfers.start(
                  {{*outcome->prepared.output},
                   Location::fromPath(target.parent_path().native()),
                   TransferAction::Move,
                   false}
              )) {
            owner->error = "Could not start the archive publication.";
            owner->clean({std::exchange(owner->stage, {})});
            owner->transferLifetime.reset();
          }
          owner->notify();
        },
        nullptr
    ));
    g_task_set_check_cancellable(task.get(), false);
    g_task_set_task_data(
        task.get(),
        new Request{
            shared_from_this(), operation, std::move(sources), std::move(destination), format, std::move(selected),
            std::move(expected)
        },
        [](gpointer data) { delete static_cast<Request*>(data); }
    );
    g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable* cancel) {
      const auto& request = *static_cast<Request*>(data);
      auto result = std::make_unique<Result>();
      try {
        auto& owner = *request.owner;
        if (request.operation == Operation::Inspect) {
          result->prepared = archive_work::inspect(request.sources.front(), owner.limits, cancel, owner.progress);
        } else if (request.operation == Operation::Compress) {
          result->prepared = archive_work::compress(
              request.sources, *request.destination, request.format, owner.limits, cancel, owner.progress
          );
        } else {
          result->prepared = archive_work::extract(
              request.sources.front(), *request.destination, request.selected, request.expected,
              request.operation == Operation::Open, owner.limits, cancel, owner.progress
          );
        }
      } catch (const std::exception& error) {
        result->error = error.what();
      }
      g_task_return_pointer(task, result.release(), [](gpointer data) { delete static_cast<Result*>(data); });
    });
    startTimer();
    notify();
    return true;
  }
};
files::ArchiveService::ArchiveService(ArchiveLimits limits) : m_state(std::make_shared<State>(limits)) {
  const std::weak_ptr<State> weak = m_state;
  m_state->transfers.setChangedCallback([weak] {
    if (const auto owner = weak.lock()) {
      owner->transferChanged();
    }
  });
}
files::ArchiveService::~ArchiveService() {
  m_state->changed = {};
  shutdown();
}
bool files::ArchiveService::inspect(Location source) {
  return m_state->start(State::Operation::Inspect, {std::move(source)}, std::nullopt, ArchiveFormat::Zip, {});
}
bool files::ArchiveService::compress(std::vector<Location> sources, Location destination, ArchiveFormat format) {
  return m_state->start(State::Operation::Compress, std::move(sources), std::move(destination), format, {});
}
bool files::ArchiveService::extract(Location source, Location destination, std::vector<std::string> selected) {
  return m_state->start(
      State::Operation::Extract, {std::move(source)}, std::move(destination), ArchiveFormat::Zip, std::move(selected)
  );
}
bool files::ArchiveService::openMember(const std::string& path) {
  if (!m_state->catalog) {
    return false;
  }
  return m_state->start(
      State::Operation::Open, {m_state->catalog->source}, m_state->catalog->source, ArchiveFormat::Zip, {path}
  );
}
void files::ArchiveService::cancel() {
  g_cancellable_cancel(m_state->cancel.get());
  m_state->transfers.cancel();
}
void files::ArchiveService::shutdown() {
  if (m_state->closing) {
    return;
  }
  m_state->closing = true;
  cancel();
  if (!busy()) {
    m_state->clean({});
  }
}
bool files::ArchiveService::busy() const { return m_state->phase != ArchivePhase::Idle; }
files::ArchivePhase files::ArchiveService::phase() const { return m_state->phase; }
std::uint64_t files::ArchiveService::bytes() const {
  return m_state->phase == ArchivePhase::Transferring ? m_state->transfers.bytes() : m_state->progress.bytes.load();
}
std::size_t files::ArchiveService::items() const { return m_state->progress.items.load(); }
const std::string& files::ArchiveService::error() const { return m_state->error; }
const std::shared_ptr<const files::ArchiveCatalog>& files::ArchiveService::catalog() const { return m_state->catalog; }
std::optional<files::Location> files::ArchiveService::takeOpenedFile() {
  return std::exchange(m_state->opened, std::nullopt);
}
const std::vector<files::TransferResult>& files::ArchiveService::results() const { return m_state->results; }
std::optional<files::TransferConflict> files::ArchiveService::conflict() const { return m_state->transfers.conflict(); }
bool files::ArchiveService::resolve(std::uint64_t id, TransferConflictChoice choice) {
  return m_state->transfers.resolve(id, choice);
}
void files::ArchiveService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
