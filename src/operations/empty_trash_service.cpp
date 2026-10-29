#include "operations/empty_trash_service.h"

#include <atomic>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace {
  constexpr const char* kAttributes =
      "standard::name,standard::display-name,standard::type,standard::is-symlink,id::file,id::filesystem,etag::value,"
      "trash::orig-path,trash::deletion-date";
  struct ErrorFree {
    void operator()(GError* error) const { g_error_free(error); }
  };
  using Error = std::unique_ptr<GError, ErrorFree>;
  std::optional<std::string> attribute(GFileInfo* info, const char* name, bool bytes = false) {
    if (g_file_info_get_attribute_type(info, name)
        != (bytes ? G_FILE_ATTRIBUTE_TYPE_BYTE_STRING : G_FILE_ATTRIBUTE_TYPE_STRING))
      return std::nullopt;
    const char* value =
        bytes ? g_file_info_get_attribute_byte_string(info, name) : g_file_info_get_attribute_string(info, name);
    return value != nullptr && *value != '\0' ? std::optional<std::string>(value) : std::nullopt;
  }
  struct Identity {
    std::string file, filesystem;
    GFileType type;
    bool symlink;
    std::optional<std::string> etag, original, deleted;
  };
  Identity identity(GFileInfo* info) {
    const auto file = attribute(info, G_FILE_ATTRIBUTE_ID_FILE);
    const auto filesystem = attribute(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
    const auto type = g_file_info_get_file_type(info);
    if (!file || !filesystem || type == G_FILE_TYPE_UNKNOWN)
      throw std::runtime_error("The trash service cannot identify these items safely.");
    return {
        *file,
        *filesystem,
        type,
        g_file_info_get_is_symlink(info) != FALSE,
        attribute(info, G_FILE_ATTRIBUTE_ETAG_VALUE),
        attribute(info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH, true),
        attribute(info, G_FILE_ATTRIBUTE_TRASH_DELETION_DATE)
    };
  }
  bool sameIdentity(const Identity& a, const Identity& b) {
    return a.file == b.file
        && a.filesystem == b.filesystem
        && a.type == b.type
        && a.symlink == b.symlink
        && (!a.etag || a.etag == b.etag)
        && a.original == b.original
        && a.deleted == b.deleted;
  }
  struct Target {
    files::Location location;
    std::string displayName;
    Identity identity;
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
        if (size > std::numeric_limits<std::uint64_t>::max() - *reply.size)
          reply.sizeIncomplete = true;
        else
          *reply.size += size;
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
      if (info)
        inspect(file.get(), info.get(), 0);
      else
        reply.sizeIncomplete = true;
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
      if (!g_file_enumerator_close(enumerator.get(), cancellable, nullptr))
        reply.sizeIncomplete = true;
    }
    return reply;
  }
  enum class WorkKind { PREPARE, MEASURE, EMPTY };
  files::EmptyTrashOutcome failedDelete(const GError* error) {
    using Outcome = files::EmptyTrashOutcome;
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
      return Outcome::SKIPPED;
    if (error != nullptr && error->domain == G_IO_ERROR) {
      switch (error->code) {
      case G_IO_ERROR_PERMISSION_DENIED:
      case G_IO_ERROR_NOT_SUPPORTED:
      case G_IO_ERROR_READ_ONLY:
      case G_IO_ERROR_NOT_EMPTY:
      case G_IO_ERROR_INVALID_ARGUMENT:
        return Outcome::FAILED;
      default:
        break;
      }
    }
    return Outcome::UNCONFIRMED;
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
    if (callback)
      callback();
  }
  static void
  launch(const std::shared_ptr<State>& owner, std::function<Reply()> work, WorkKind kind = WorkKind::PREPARE) {
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
          if (request->kind == WorkKind::MEASURE) {
            owner->snapshot.size = reply->size;
            owner->snapshot.sizeIncomplete = reply->sizeIncomplete || !reply->error.empty();
            if (std::exchange(owner->emptyRequested, false)) {
              owner->cancellable.reset(g_cancellable_new());
              startEmpty(owner);
              return;
            }
            if (g_cancellable_is_cancelled(owner->cancellable.get())) {
              owner->snapshot.status = EmptyTrashStatus::CANCELLED;
              owner->targets.reset();
            }
            owner->notify();
            return;
          }
          owner->snapshot.error = std::move(reply->error);
          if (reply->finished) {
            owner->snapshot.results = std::move(reply->results);
            owner->snapshot.status = EmptyTrashStatus::FINISHED;
            owner->targets.reset();
          } else if (g_cancellable_is_cancelled(owner->cancellable.get())) {
            owner->snapshot.status = EmptyTrashStatus::CANCELLED;
            owner->targets.reset();
          } else if (!owner->snapshot.error.empty()) {
            owner->snapshot.status = EmptyTrashStatus::ERROR;
            owner->targets.reset();
          } else {
            owner->targets = std::move(reply->targets);
            owner->snapshot.results = std::move(reply->results);
            owner->snapshot.status = EmptyTrashStatus::READY;
            launch(
                owner, [owner, targets = owner->targets] { return measureSize(*targets, owner->cancellable.get()); },
                WorkKind::MEASURE
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
        if (!g_cancellable_is_cancelled(request.owner->cancellable.get()))
          *reply = request.work();
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
bool files::EmptyTrashService::pending() const { return busy() || m_state->snapshot.status == EmptyTrashStatus::READY; }
bool files::EmptyTrashService::canEmpty() const {
  return m_state->snapshot.status == EmptyTrashStatus::READY && m_state->targets && !m_state->targets->empty();
}
std::size_t files::EmptyTrashService::completed() const { return m_state->completed.load(); }
const files::EmptyTrashSnapshot& files::EmptyTrashService::snapshot() const { return m_state->snapshot; }
void files::EmptyTrashService::cancel() {
  m_state->emptyRequested = false;
  g_cancellable_cancel(m_state->cancellable.get());
  m_state->targets.reset();
  if (!busy() && m_state->snapshot.status != EmptyTrashStatus::FINISHED)
    m_state->snapshot.status = EmptyTrashStatus::CANCELLED;
}
bool files::EmptyTrashService::prepare() {
  if (busy())
    return false;
  m_state->targets.reset();
  m_state->cancellable.reset(g_cancellable_new());
  m_state->completed.store(0);
  m_state->snapshot = {
      .status = EmptyTrashStatus::PREPARING, .results = {}, .error = {}, .size = std::nullopt, .sizeIncomplete = false
  };
  State::launch(m_state, [owner = m_state] {
    const auto root = Location::trash().file();
    GError* raw = nullptr;
    const GioPtr<GFileEnumerator> enumerator(g_file_enumerate_children(
        root.get(), kAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, owner->cancellable.get(), &raw
    ));
    const Error error(raw);
    if (!enumerator)
      throw std::runtime_error(error ? error->message : "Could not read Trash.");
    auto targets = std::make_shared<Targets>();
    std::set<std::string> names;
    for (;;) {
      raw = nullptr;
      const GioPtr<GFileInfo> info(g_file_enumerator_next_file(enumerator.get(), owner->cancellable.get(), &raw));
      const Error nextError(raw);
      if (nextError)
        throw std::runtime_error(nextError->message);
      if (!info)
        break;
      const char* name = g_file_info_get_name(info.get());
      if (name == nullptr
          || *name == '\0'
          || std::string_view(name).contains('/')
          || std::string_view(name) == "."
          || std::string_view(name) == ".."
          || !names.insert(name).second)
        throw std::runtime_error("The trash service returned an invalid item.");
      const GioPtr<GFile> child(g_file_get_child(root.get(), name));
      const GioPtr<GFile> parent(g_file_get_parent(child.get()));
      const GString uri(g_file_get_uri(child.get()));
      auto location = Location::fromUri(uri.get());
      if (!location.isTrash() || !parent || !g_file_equal(parent.get(), root.get()))
        throw std::runtime_error("The trash service returned an item outside Trash.");
      const char* display = g_file_info_get_display_name(info.get());
      const GString valid(g_utf8_make_valid(display != nullptr ? display : name, -1));
      targets->push_back({std::move(location), valid.get(), identity(info.get())});
    }
    raw = nullptr;
    const bool closed = g_file_enumerator_close(enumerator.get(), owner->cancellable.get(), &raw);
    const Error closeError(raw);
    if (!closed)
      throw std::runtime_error(closeError ? closeError->message : "Could not finish reading Trash.");
    Reply reply{.targets = std::move(targets), .results = {}, .error = {}, .finished = false};
    for (const auto& target : *reply.targets)
      reply.results.push_back({target.location, target.displayName, EmptyTrashOutcome::CANCELLED, {}});
    return reply;
  });
  return true;
}
bool files::EmptyTrashService::empty() {
  if (!canEmpty())
    return false;
  m_state->snapshot.status = EmptyTrashStatus::EMPTYING;
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
        for (const auto& target : *targets)
          reply.results.push_back({target.location, target.displayName, EmptyTrashOutcome::CANCELLED, {}});
        for (std::size_t index = 0; index < targets->size(); ++index) {
          if (g_cancellable_is_cancelled(owner->cancellable.get()))
            break;
          const auto& target = (*targets)[index];
          auto& result = reply.results[index];
          try {
            GError* raw = nullptr;
            const GioPtr<GFileInfo> info(g_file_query_info(
                target.location.file().get(), kAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                owner->cancellable.get(), &raw
            ));
            const Error error(raw);
            if (!info) {
              if (g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED))
                break;
              result.outcome = g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_NOT_FOUND)
                  ? EmptyTrashOutcome::SKIPPED
                  : EmptyTrashOutcome::FAILED;
              result.error = error ? error->message : "Could not check the item.";
            } else if (!sameIdentity(target.identity, identity(info.get()))) {
              result.outcome = EmptyTrashOutcome::FAILED;
              result.error = "The item changed after confirmation was prepared. It was left in Trash.";
            } else {
              if (g_cancellable_is_cancelled(owner->cancellable.get()))
                break;
              raw = nullptr;
              // Finish the current provider request before observing cancellation.
              const bool removed = g_file_delete(target.location.file().get(), nullptr, &raw);
              const Error deleteError(raw);
              result.outcome = removed ? EmptyTrashOutcome::DELETED : failedDelete(deleteError.get());
              if (!removed)
                result.error = deleteError ? deleteError->message : "Could not confirm deletion.";
            }
          } catch (const std::exception& error) {
            result.outcome = EmptyTrashOutcome::FAILED;
            result.error = error.what();
          }
          owner->completed.fetch_add(1);
        }
        return reply;
      },
      WorkKind::EMPTY
  );
}
