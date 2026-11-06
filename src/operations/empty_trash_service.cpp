#include "operations/empty_trash_service.h"

#include "operations/trash_item.h"

#include <atomic>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace {
  struct ErrorFree {
    void operator()(GError* error) const { g_error_free(error); }
  };
  using Error = std::unique_ptr<GError, ErrorFree>;
  struct Target {
    files::Location location;
    std::string displayName;
    files::TrashItemIdentity identity;
  };
  using Targets = std::vector<Target>;
  struct Reply {
    std::shared_ptr<Targets> targets;
    std::vector<files::EmptyTrashResult> results;
    std::string error;
    bool finished = false;
    std::optional<std::uint64_t> size = std::nullopt;
    bool sizeIncomplete = false;
  };
  Reply measureSize(const Targets& targets, GCancellable* cancellable) {
    struct Folder {
      files::Location location;
      unsigned int depth;
    };
    Reply reply;
    reply.size = 0;
    std::vector<Folder> pending;
    constexpr const char* attributes = "standard::name,standard::type,standard::is-symlink,standard::size";
    const auto inspect = [&](GFile* file, GFileInfo* info, unsigned int depth) {
      const auto type = g_file_info_get_file_type(info);
      if (type == G_FILE_TYPE_DIRECTORY && !g_file_info_get_is_symlink(info)) {
        if (depth >= 256 || pending.size() >= 65536) {
          reply.sizeIncomplete = true;
        } else {
          const files::GString uri(g_file_get_uri(file));
          pending.push_back({files::Location::fromUri(uri.get()), depth});
        }
      } else if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_STANDARD_SIZE) == G_FILE_ATTRIBUTE_TYPE_UINT64) {
        const auto size = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_STANDARD_SIZE);
        if (size > std::numeric_limits<std::uint64_t>::max() - *reply.size) {
          reply.sizeIncomplete = true;
        } else {
          *reply.size += size;
        }
      } else {
        reply.sizeIncomplete = true;
      }
    };
    for (const auto& target : targets) {
      if (g_cancellable_is_cancelled(cancellable)) {
        reply.sizeIncomplete = true;
        return reply;
      }
      const auto file = target.location.file();
      const files::GioPtr<GFileInfo> info(
          g_file_query_info(file.get(), attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr)
      );
      if (info) {
        inspect(file.get(), info.get(), 0);
      } else {
        reply.sizeIncomplete = true;
      }
    }
    while (!pending.empty()) {
      if (g_cancellable_is_cancelled(cancellable)) {
        reply.sizeIncomplete = true;
        return reply;
      }
      const auto folder = std::move(pending.back());
      pending.pop_back();
      const auto file = folder.location.file();
      const files::GioPtr<GFileEnumerator> enumerator(
          g_file_enumerate_children(file.get(), attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr)
      );
      if (!enumerator) {
        reply.sizeIncomplete = true;
        continue;
      }
      for (;;) {
        GError* raw = nullptr;
        const files::GioPtr<GFileInfo> info(g_file_enumerator_next_file(enumerator.get(), cancellable, &raw));
        const Error error(raw);
        if (!info) {
          reply.sizeIncomplete = reply.sizeIncomplete || error != nullptr;
          break;
        }
        const char* name = g_file_info_get_name(info.get());
        if (name == nullptr
            || *name == '\0'
            || std::string_view(name).contains('/')
            || std::string_view(name) == "."
            || std::string_view(name) == "..") {
          reply.sizeIncomplete = true;
          continue;
        }
        const files::GioPtr<GFile> child(g_file_get_child(file.get(), name));
        inspect(child.get(), info.get(), folder.depth + 1);
      }
      if (!g_file_enumerator_close(enumerator.get(), cancellable, nullptr)) {
        reply.sizeIncomplete = true;
      }
    }
    return reply;
  }
  enum class WorkKind { Prepare, Measure, Empty };
  files::EmptyTrashOutcome failedDelete(const GError* error) {
    using Outcome = files::EmptyTrashOutcome;
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
      return Outcome::Skipped;
    }
    if (error != nullptr && error->domain == G_IO_ERROR) {
      switch (error->code) {
      case G_IO_ERROR_PERMISSION_DENIED:
      case G_IO_ERROR_NOT_SUPPORTED:
      case G_IO_ERROR_READ_ONLY:
      case G_IO_ERROR_NOT_EMPTY:
      case G_IO_ERROR_INVALID_ARGUMENT:
        return Outcome::Failed;
      default:
        break;
      }
    }
    return Outcome::Unconfirmed;
  }
} // namespace

struct files::EmptyTrashService::State {
  EmptyTrashSnapshot snapshot;
  std::shared_ptr<Targets> targets;
  GioPtr<GCancellable> cancellable{g_cancellable_new()};
  std::atomic<std::size_t> completed{0};
  bool active = false;
  bool emptyRequested = false;
  static void startEmpty(const std::shared_ptr<State>& owner);
  std::function<void()> changed;
  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  static void
  launch(const std::shared_ptr<State>& owner, std::function<Reply()> work, WorkKind kind = WorkKind::Prepare) {
    struct Request {
      std::shared_ptr<State> owner;
      std::function<Reply()> work;
      WorkKind kind;
    };
    owner->active = true;
    GioPtr<GTask> task(g_task_new(
        nullptr, nullptr,
        [](GObject*, GAsyncResult* result, gpointer) {
          const auto* request = static_cast<Request*>(g_task_get_task_data(G_TASK(result)));
          const auto owner = request->owner;
          std::unique_ptr<Reply> reply(static_cast<Reply*>(g_task_propagate_pointer(G_TASK(result), nullptr)));
          owner->active = false;
          if (request->kind == WorkKind::Measure) {
            owner->snapshot.size = reply->size;
            owner->snapshot.sizeIncomplete = reply->sizeIncomplete || !reply->error.empty();
            if (std::exchange(owner->emptyRequested, false)) {
              owner->cancellable.reset(g_cancellable_new());
              startEmpty(owner);
              return;
            }
            if (g_cancellable_is_cancelled(owner->cancellable.get())) {
              owner->snapshot.status = EmptyTrashStatus::Cancelled;
              owner->targets.reset();
            }
            owner->notify();
            return;
          }
          owner->snapshot.error = std::move(reply->error);
          if (reply->finished) {
            owner->snapshot.results = std::move(reply->results);
            owner->snapshot.status = EmptyTrashStatus::Finished;
            owner->targets.reset();
          } else if (g_cancellable_is_cancelled(owner->cancellable.get())) {
            owner->snapshot.status = EmptyTrashStatus::Cancelled;
            owner->targets.reset();
          } else if (!owner->snapshot.error.empty()) {
            owner->snapshot.status = EmptyTrashStatus::Error;
            owner->targets.reset();
          } else {
            owner->targets = std::move(reply->targets);
            owner->snapshot.results = std::move(reply->results);
            owner->snapshot.status = EmptyTrashStatus::Ready;
            launch(
                owner, [owner, targets = owner->targets] { return measureSize(*targets, owner->cancellable.get()); },
                WorkKind::Measure
            );
            return;
          }
          owner->notify();
        },
        nullptr
    ));
    g_task_set_task_data(task.get(), new Request{owner, std::move(work), kind}, [](gpointer data) {
      delete static_cast<Request*>(data);
    });
    g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable*) {
      auto& request = *static_cast<Request*>(data);
      auto reply = std::make_unique<Reply>();
      try {
        if (!g_cancellable_is_cancelled(request.owner->cancellable.get())) {
          *reply = request.work();
        }
      } catch (const std::exception& error) {
        reply->error = error.what();
      }
      g_task_return_pointer(task, reply.release(), [](gpointer data) { delete static_cast<Reply*>(data); });
    });
    owner->notify();
  }
};

files::EmptyTrashService::EmptyTrashService() : m_state(std::make_shared<State>()) {}
files::EmptyTrashService::~EmptyTrashService() {
  m_state->changed = {};
  cancel();
}
void files::EmptyTrashService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
bool files::EmptyTrashService::busy() const { return m_state->active; }
bool files::EmptyTrashService::pending() const { return busy() || m_state->snapshot.status == EmptyTrashStatus::Ready; }
bool files::EmptyTrashService::canEmpty() const {
  return m_state->snapshot.status == EmptyTrashStatus::Ready && m_state->targets && !m_state->targets->empty();
}
std::size_t files::EmptyTrashService::completed() const { return m_state->completed.load(); }
const files::EmptyTrashSnapshot& files::EmptyTrashService::snapshot() const { return m_state->snapshot; }
void files::EmptyTrashService::cancel() {
  m_state->emptyRequested = false;
  g_cancellable_cancel(m_state->cancellable.get());
  m_state->targets.reset();
  if (!busy() && m_state->snapshot.status != EmptyTrashStatus::Finished) {
    m_state->snapshot.status = EmptyTrashStatus::Cancelled;
  }
}
bool files::EmptyTrashService::prepare() { return prepareTargets(std::nullopt); }
bool files::EmptyTrashService::prepareSelection(std::vector<DirectoryEntry> selection) {
  if (selection.empty() || selection.size() > 4096) {
    return false;
  }
  return prepareTargets(std::move(selection));
}
bool files::EmptyTrashService::prepareTargets(std::optional<std::vector<DirectoryEntry>> selection) {
  if (busy()) {
    return false;
  }
  m_state->targets.reset();
  m_state->cancellable.reset(g_cancellable_new());
  m_state->completed.store(0);
  m_state->snapshot = {
      .status = EmptyTrashStatus::Preparing, .results = {}, .error = {}, .size = std::nullopt, .sizeIncomplete = false
  };
  State::launch(m_state, [owner = m_state, selection = std::move(selection)] {
    auto targets = std::make_shared<Targets>();
    if (selection) {
      std::set<std::string> seen;
      for (const auto& entry : *selection) {
        if (!isTopLevelTrashItem(entry.location) || !seen.insert(entry.location.uri()).second) {
          throw std::runtime_error("Only distinct top-level Trash items can be deleted.");
        }
        GError* raw = nullptr;
        const GioPtr<GFileInfo> info(g_file_query_info(
            entry.location.file().get(), trashItemAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
            owner->cancellable.get(), &raw
        ));
        const Error error(raw);
        if (!info) {
          throw std::runtime_error(error ? error->message : "Could not read the selected item.");
        }
        const auto expected = readTrashItemIdentity(entry);
        if (expected != readTrashItemIdentity(info.get())) {
          throw std::runtime_error("The selected item changed. Select it again before deleting it.");
        }
        targets->push_back({entry.location, entry.displayName, expected});
      }
      Reply reply{.targets = std::move(targets), .results = {}, .error = {}, .finished = false};
      for (const auto& target : *reply.targets) {
        reply.results.push_back({target.location, target.displayName, EmptyTrashOutcome::Cancelled, {}});
      }
      return reply;
    }
    const auto root = Location::trash().file();
    GError* raw = nullptr;
    const GioPtr<GFileEnumerator> enumerator(g_file_enumerate_children(
        root.get(), trashItemAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, owner->cancellable.get(), &raw
    ));
    const Error error(raw);
    if (!enumerator) {
      throw std::runtime_error(error ? error->message : "Could not read Trash.");
    }
    std::set<std::string> names;
    for (;;) {
      raw = nullptr;
      const GioPtr<GFileInfo> info(g_file_enumerator_next_file(enumerator.get(), owner->cancellable.get(), &raw));
      const Error nextError(raw);
      if (nextError) {
        throw std::runtime_error(nextError->message);
      }
      if (!info) {
        break;
      }
      const char* name = g_file_info_get_name(info.get());
      if (name == nullptr
          || *name == '\0'
          || std::string_view(name).contains('/')
          || std::string_view(name) == "."
          || std::string_view(name) == ".."
          || !names.insert(name).second) {
        throw std::runtime_error("The trash service returned an invalid item.");
      }
      const GioPtr<GFile> child(g_file_get_child(root.get(), name));
      const GioPtr<GFile> parent(g_file_get_parent(child.get()));
      const GString uri(g_file_get_uri(child.get()));
      auto location = Location::fromUri(uri.get());
      if (!location.isTrash() || !parent || !g_file_equal(parent.get(), root.get())) {
        throw std::runtime_error("The trash service returned an item outside Trash.");
      }
      const char* display = g_file_info_get_display_name(info.get());
      const GString valid(g_utf8_make_valid(display != nullptr ? display : name, -1));
      targets->push_back({std::move(location), valid.get(), readTrashItemIdentity(info.get())});
    }
    raw = nullptr;
    const bool closed = g_file_enumerator_close(enumerator.get(), owner->cancellable.get(), &raw);
    const Error closeError(raw);
    if (!closed) {
      throw std::runtime_error(closeError ? closeError->message : "Could not finish reading Trash.");
    }
    Reply reply{.targets = std::move(targets), .results = {}, .error = {}, .finished = false};
    for (const auto& target : *reply.targets) {
      reply.results.push_back({target.location, target.displayName, EmptyTrashOutcome::Cancelled, {}});
    }
    return reply;
  });
  return true;
}
bool files::EmptyTrashService::empty() {
  if (!canEmpty()) {
    return false;
  }
  m_state->snapshot.status = EmptyTrashStatus::Emptying;
  if (busy()) {
    m_state->emptyRequested = true;
    g_cancellable_cancel(m_state->cancellable.get());
    m_state->notify();
  } else {
    m_state->cancellable.reset(g_cancellable_new());
    State::startEmpty(m_state);
  }
  return true;
}
void files::EmptyTrashService::State::startEmpty(const std::shared_ptr<State>& owner) {
  launch(
      owner,
      [owner, targets = owner->targets] {
        Reply reply{.targets = {}, .results = {}, .error = {}, .finished = true};
        for (const auto& target : *targets) {
          reply.results.push_back({target.location, target.displayName, EmptyTrashOutcome::Cancelled, {}});
        }
        for (std::size_t index = 0; index < targets->size(); ++index) {
          if (g_cancellable_is_cancelled(owner->cancellable.get())) {
            break;
          }
          const auto& target = (*targets)[index];
          auto& result = reply.results[index];
          try {
            GError* raw = nullptr;
            const GioPtr<GFileInfo> info(g_file_query_info(
                target.location.file().get(), trashItemAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                owner->cancellable.get(), &raw
            ));
            const Error error(raw);
            if (!info) {
              if (g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                break;
              }
              result.outcome = g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_NOT_FOUND)
                  ? EmptyTrashOutcome::Skipped
                  : EmptyTrashOutcome::Failed;
              result.error = error ? error->message : "Could not check the item.";
            } else if (target.identity != readTrashItemIdentity(info.get())) {
              result.outcome = EmptyTrashOutcome::Failed;
              result.error = "The item changed after confirmation was prepared. It was left in Trash.";
            } else {
              if (g_cancellable_is_cancelled(owner->cancellable.get())) {
                break;
              }
              raw = nullptr;
              // Finish the current provider request before observing cancellation.
              const bool removed = g_file_delete(target.location.file().get(), nullptr, &raw);
              const Error deleteError(raw);
              result.outcome = removed ? EmptyTrashOutcome::Deleted : failedDelete(deleteError.get());
              if (!removed) {
                result.error = deleteError ? deleteError->message : "Could not confirm deletion.";
              }
            }
          } catch (const std::exception& error) {
            result.outcome = EmptyTrashOutcome::Failed;
            result.error = error.what();
          }
          owner->completed.fetch_add(1);
        }
        return reply;
      },
      WorkKind::Empty
  );
}
