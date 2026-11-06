#include "operations/restore_service.h"

#include "operations/trash_item.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace {
  struct ErrorFree {
    void operator()(GError* error) const { g_error_free(error); }
  };
  using Error = std::unique_ptr<GError, ErrorFree>;
  files::GioPtr<GFileInfo> query(const files::Location& location, GCancellable* cancel, bool follow = false) {
    GError* raw = nullptr;
    files::GioPtr<GFileInfo> info(g_file_query_info(
        location.file().get(), files::trashItemAttributes,
        follow ? G_FILE_QUERY_INFO_NONE : G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, &raw
    ));
    const Error error(raw);
    if (!info) {
      throw std::runtime_error(error ? error->message : "Could not read the item.");
    }
    return info;
  }
  files::Location parentOf(const files::Location& location) {
    const files::GioPtr<GFile> parent(g_file_get_parent(location.file().get()));
    if (!parent) {
      throw std::runtime_error("The original folder is unavailable.");
    }
    const files::GString uri(g_file_get_uri(parent.get()));
    return files::Location::fromUri(uri.get());
  }
  files::Location numbered(const files::Location& original, bool directory, std::size_t number) {
    const auto parent = parentOf(original);
    const files::GString name(g_file_get_basename(original.file().get()));
    const std::string base(name.get());
    const auto dot = base.find_last_of('.');
    const bool extension = !directory && dot != std::string::npos && dot != 0;
    auto stem = extension ? base.substr(0, dot) : base;
    const auto suffix = extension ? base.substr(dot) : std::string{};
    const auto counter = " (" + std::to_string(number) + ")";
    const auto limit = ::pathconf(parent.nativePath()->c_str(), _PC_NAME_MAX);
    if (limit > 0 && stem.size() + counter.size() + suffix.size() > static_cast<std::size_t>(limit)) {
      if (counter.size() + suffix.size() >= static_cast<std::size_t>(limit)) {
        throw std::runtime_error("The renamed file name would be too long.");
      }
      auto bytes = static_cast<std::size_t>(limit) - counter.size() - suffix.size();
      if (g_utf8_validate(stem.data(), static_cast<gssize>(stem.size()), nullptr)) {
        while (bytes > 0 && (static_cast<unsigned char>(stem[bytes]) & 0xc0U) == 0x80U) {
          --bytes;
        }
      }
      stem.resize(bytes);
    }
    const files::GioPtr<GFile> child(g_file_get_child(parent.file().get(), (stem + counter + suffix).c_str()));
    const files::GString uri(g_file_get_uri(child.get()));
    return files::Location::fromUri(uri.get());
  }
  bool collision(const GError* error) {
    return g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS)
        || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE)
        || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY);
  }
  files::RestoreOutcome failedMove(const GError* error) {
    if (error && error->domain == G_IO_ERROR) {
      switch (error->code) {
      case G_IO_ERROR_NOT_FOUND:
      case G_IO_ERROR_PERMISSION_DENIED:
      case G_IO_ERROR_NOT_SUPPORTED:
      case G_IO_ERROR_READ_ONLY:
      case G_IO_ERROR_INVALID_ARGUMENT:
      case G_IO_ERROR_FILENAME_TOO_LONG:
      case G_IO_ERROR_WOULD_RECURSE:
      case G_IO_ERROR_NOT_DIRECTORY:
      case G_IO_ERROR_NO_SPACE:
        return files::RestoreOutcome::Failed;
      default:
        break;
      }
    }
    return files::RestoreOutcome::Unconfirmed;
  }
} // namespace

struct files::RestoreService::State {
  GioPtr<GCancellable> cancel{g_cancellable_new()};
  bool active = false;
  std::size_t total = 0;
  std::atomic<std::size_t> completed{0};
  std::vector<RestoreResult> results;
  std::function<void()> changed;
  mutable std::mutex mutex;
  std::condition_variable decision;
  std::optional<TransferConflict> conflict;
  std::optional<TransferConflictChoice> choice;
  std::uint64_t nextConflict = 0;

  static void notify(const std::shared_ptr<State>& owner) {
    auto* source = g_idle_source_new();
    g_source_set_callback(
        source,
        [](gpointer data) -> gboolean {
          const auto& state = *static_cast<std::shared_ptr<State>*>(data);
          const auto callback = state->changed;
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
  static TransferConflictChoice
  ask(const std::shared_ptr<State>& owner, const DirectoryEntry& entry, const Location& destination,
      GFileInfo* existing) {
    const auto size = g_file_info_has_attribute(existing, G_FILE_ATTRIBUTE_STANDARD_SIZE)
        ? std::optional(g_file_info_get_attribute_uint64(existing, G_FILE_ATTRIBUTE_STANDARD_SIZE))
        : std::nullopt;
    const auto modified = g_file_info_has_attribute(existing, G_FILE_ATTRIBUTE_TIME_MODIFIED)
        ? std::optional(g_file_info_get_attribute_uint64(existing, G_FILE_ATTRIBUTE_TIME_MODIFIED))
        : std::nullopt;
    const GString name(g_file_get_basename(destination.file().get()));
    const GString display(g_filename_display_name(name.get()));
    std::unique_lock lock(owner->mutex);
    owner->choice.reset();
    owner->conflict = TransferConflict{
        ++owner->nextConflict,
        entry.location,
        destination,
        display.get(),
        entry.type == G_FILE_TYPE_DIRECTORY,
        g_file_info_get_file_type(existing) == G_FILE_TYPE_DIRECTORY,
        entry.size,
        size,
        entry.modifiedSeconds,
        modified
    };
    lock.unlock();
    notify(owner);
    lock.lock();
    owner->decision.wait(lock, [&] { return g_cancellable_is_cancelled(owner->cancel.get()) || owner->choice; });
    const auto choice =
        g_cancellable_is_cancelled(owner->cancel.get()) ? TransferConflictChoice::Cancel : *owner->choice;
    owner->conflict.reset();
    owner->choice.reset();
    return choice;
  }
  static void restore(const std::shared_ptr<State>& owner, const DirectoryEntry& entry, RestoreResult& result) {
    const auto expected = readTrashItemIdentity(entry);
    if (!isTopLevelTrashItem(entry.location) || !entry.trash.original || !entry.trash.original->nativePath()) {
      throw std::runtime_error("This item has no usable original location.");
    }
    const auto original = *entry.trash.original;
    auto destination = original;
    const auto parent = parentOf(original);
    const auto parentInfo = query(parent, owner->cancel.get(), true);
    const auto parentIdentity = readFileIdentity(parentInfo.get());
    if (!parentIdentity || g_file_info_get_file_type(parentInfo.get()) != G_FILE_TYPE_DIRECTORY) {
      throw std::runtime_error("The original folder is unavailable.");
    }
    std::optional<TrashItemIdentity> replace;
    std::size_t number = 0;
    for (std::size_t attempt = 0; attempt < 10000; ++attempt) {
      if (g_cancellable_is_cancelled(owner->cancel.get())) {
        return;
      }
      const auto current = query(entry.location, owner->cancel.get());
      if (expected != readTrashItemIdentity(current.get())) {
        throw std::runtime_error("The selected item changed. It was left in Trash.");
      }
      const auto currentParent = query(parent, owner->cancel.get(), true);
      if (readFileIdentity(currentParent.get()) != parentIdentity) {
        throw std::runtime_error("The original folder changed. The item was left in Trash.");
      }
      if (replace && *replace != readTrashItemIdentity(query(destination, owner->cancel.get()).get())) {
        replace.reset();
      }
      result.destination = destination;
      if (g_cancellable_is_cancelled(owner->cancel.get())) {
        return;
      }
      GError* raw = nullptr;
      auto flags = static_cast<GFileCopyFlags>(
          G_FILE_COPY_NOFOLLOW_SYMLINKS
          | G_FILE_COPY_NO_FALLBACK_FOR_MOVE
          | (replace ? G_FILE_COPY_OVERWRITE : G_FILE_COPY_NONE)
      );
      // Observe cancellation between complete provider moves, never midway through publication.
      const bool moved =
          g_file_move(entry.location.file().get(), destination.file().get(), flags, nullptr, nullptr, nullptr, &raw);
      const Error error(raw);
      if (moved) {
        result.outcome = RestoreOutcome::Restored;
        return;
      }
      if (!collision(error.get())) {
        result.outcome = failedMove(error.get());
        result.error = error ? error->message : "Could not confirm restoration.";
        return;
      }
      replace.reset();
      if (number > 0) {
        destination = numbered(original, entry.type == G_FILE_TYPE_DIRECTORY, ++number);
        continue;
      }
      const auto existing = query(destination, owner->cancel.get());
      const auto choice = ask(owner, entry, destination, existing.get());
      if (choice == TransferConflictChoice::Cancel) {
        return;
      }
      if (choice == TransferConflictChoice::Skip) {
        result.outcome = RestoreOutcome::Skipped;
        return;
      }
      if (choice == TransferConflictChoice::KeepBoth) {
        destination = numbered(original, entry.type == G_FILE_TYPE_DIRECTORY, ++number);
      } else if (
          entry.type != G_FILE_TYPE_DIRECTORY && g_file_info_get_file_type(existing.get()) != G_FILE_TYPE_DIRECTORY
      ) {
        replace = readTrashItemIdentity(existing.get());
      } else {
        throw std::runtime_error("Folder replacement is unavailable. Keep both or skip the item.");
      }
    }
    throw std::runtime_error("Too many name collisions. The item was left in Trash.");
  }
};

files::RestoreService::RestoreService() : m_state(std::make_shared<State>()) {}
files::RestoreService::~RestoreService() {
  m_state->changed = {};
  cancel();
}
bool files::RestoreService::busy() const { return m_state->active; }
std::size_t files::RestoreService::total() const { return m_state->total; }
std::size_t files::RestoreService::completed() const { return m_state->completed.load(); }
const std::vector<files::RestoreResult>& files::RestoreService::results() const { return m_state->results; }
void files::RestoreService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
void files::RestoreService::cancel() {
  g_cancellable_cancel(m_state->cancel.get());
  m_state->decision.notify_all();
}
std::optional<files::TransferConflict> files::RestoreService::conflict() const {
  const std::scoped_lock lock(m_state->mutex);
  return m_state->conflict;
}
bool files::RestoreService::resolve(std::uint64_t id, TransferConflictChoice choice) {
  {
    const std::scoped_lock lock(m_state->mutex);
    if (!m_state->conflict || m_state->conflict->id != id || m_state->choice) {
      return false;
    }
    m_state->choice = choice;
    if (choice == TransferConflictChoice::Cancel) {
      g_cancellable_cancel(m_state->cancel.get());
    }
  }
  m_state->decision.notify_all();
  return true;
}
bool files::RestoreService::start(std::vector<DirectoryEntry> selection) {
  if (busy() || selection.empty() || selection.size() > 4096) {
    return false;
  }
  std::set<std::string> sources;
  for (const auto& entry : selection) {
    if (!isTopLevelTrashItem(entry.location) || !sources.insert(entry.location.uri()).second) {
      return false;
    }
  }
  m_state->active = true;
  m_state->total = selection.size();
  m_state->completed.store(0);
  m_state->results.clear();
  m_state->cancel.reset(g_cancellable_new());
  struct Request {
    std::shared_ptr<State> owner;
    std::vector<DirectoryEntry> selection;
  };
  const GioPtr<GTask> task(g_task_new(
      nullptr, nullptr,
      [](GObject*, GAsyncResult* result, gpointer) {
        const auto owner = static_cast<Request*>(g_task_get_task_data(G_TASK(result)))->owner;
        const std::unique_ptr<std::vector<RestoreResult>> reply(
            static_cast<std::vector<RestoreResult>*>(g_task_propagate_pointer(G_TASK(result), nullptr))
        );
        owner->results = std::move(*reply);
        owner->active = false;
        const auto callback = owner->changed;
        if (callback) {
          callback();
        }
      },
      nullptr
  ));
  g_task_set_task_data(task.get(), new Request{m_state, std::move(selection)}, [](gpointer p) {
    delete static_cast<Request*>(p);
  });
  g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable*) {
    const auto& request = *static_cast<Request*>(data);
    auto reply = std::make_unique<std::vector<RestoreResult>>();
    std::set<std::string> seen;
    for (const auto& entry : request.selection) {
      reply->push_back({entry.location, std::nullopt, entry.displayName, RestoreOutcome::Cancelled, {}});
    }
    for (std::size_t index = 0; index < request.selection.size(); ++index) {
      if (g_cancellable_is_cancelled(request.owner->cancel.get())) {
        break;
      }
      const auto& entry = request.selection[index];
      auto& result = (*reply)[index];
      try {
        if (!seen.insert(entry.location.uri()).second) {
          throw std::runtime_error("The restore selection contains a repeated item.");
        }
        State::restore(request.owner, entry, result);
      } catch (const std::exception& error) {
        if (!g_cancellable_is_cancelled(request.owner->cancel.get())) {
          result.outcome = RestoreOutcome::Failed;
          result.error = error.what();
        }
      }
      request.owner->completed.fetch_add(1);
    }
    g_task_return_pointer(task, reply.release(), [](gpointer p) {
      delete static_cast<std::vector<RestoreResult>*>(p);
    });
  });
  State::notify(m_state);
  return true;
}
