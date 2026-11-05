#include "operations/trash_service.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
  class Fd {
  public:
    explicit Fd(int fd) : m_fd(fd) {
      if (fd < 0) {
        throw std::runtime_error(std::strerror(errno));
      }
    }
    ~Fd() { ::close(m_fd); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return m_fd; }

  private:
    int m_fd;
  };
  struct stat metadata(int fd) {
    struct stat result{};
    if (::fstat(fd, &result) != 0) {
      throw std::runtime_error(std::strerror(errno));
    }
    return result;
  }
  bool sameIdentity(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && (a.st_mode & S_IFMT) == (b.st_mode & S_IFMT);
  }
  struct Target {
    files::Location location;
    std::string name;
    Fd item;
    Target(files::Location location, std::string name, int parent)
        : location(std::move(location)), name(std::move(name)),
          item(::openat(parent, this->name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC)) {}
  };
  struct Prepared {
    std::string path;
    Fd parent;
    std::vector<std::unique_ptr<Target>> targets;
    explicit Prepared(std::string path)
        : path(std::move(path)), parent(::open(this->path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {}
  };
  struct Reply {
    std::shared_ptr<Prepared> prepared;
    std::vector<files::TrashResult> results;
    std::string error;
    bool finished = false;
  };
} // namespace

struct files::TrashService::State {
  TrashSnapshot snapshot;
  std::shared_ptr<Prepared> prepared;
  GioPtr<GCancellable> cancellable{g_cancellable_new()};
  std::atomic<std::size_t> completed{0};
  bool active = false;
  std::function<void()> changed;
  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  static void launch(const std::shared_ptr<State>& owner, std::function<Reply()> work) {
    struct Request {
      std::shared_ptr<State> owner;
      std::function<Reply()> work;
    };
    owner->active = true;
    GioPtr<GTask> task(g_task_new(
        nullptr, nullptr,
        [](GObject*, GAsyncResult* result, gpointer) {
          const auto owner = static_cast<Request*>(g_task_get_task_data(G_TASK(result)))->owner;
          std::unique_ptr<Reply> reply(static_cast<Reply*>(g_task_propagate_pointer(G_TASK(result), nullptr)));
          owner->active = false;
          owner->snapshot.error = std::move(reply->error);
          if (reply->finished) {
            owner->snapshot.results = std::move(reply->results);
            owner->snapshot.status = TrashStatus::Finished;
            owner->prepared.reset();
          } else if (g_cancellable_is_cancelled(owner->cancellable.get())) {
            owner->snapshot.status = TrashStatus::Cancelled;
            owner->prepared.reset();
          } else if (!owner->snapshot.error.empty()) {
            owner->snapshot.status = TrashStatus::Error;
            owner->prepared.reset();
          } else {
            owner->prepared = std::move(reply->prepared);
            owner->snapshot.status = TrashStatus::Ready;
          }
          owner->notify();
        },
        nullptr
    ));
    g_task_set_task_data(task.get(), new Request{owner, std::move(work)}, [](gpointer data) {
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

files::TrashService::TrashService() : m_state(std::make_shared<State>()) {}
files::TrashService::~TrashService() {
  m_state->changed = {};
  cancel();
}
void files::TrashService::setChangedCallback(std::function<void()> callback) { m_state->changed = std::move(callback); }
bool files::TrashService::busy() const { return m_state->active; }
bool files::TrashService::canTrash() const { return !busy() && m_state->prepared != nullptr; }
std::size_t files::TrashService::completed() const { return m_state->completed.load(); }
const files::TrashSnapshot& files::TrashService::snapshot() const { return m_state->snapshot; }
void files::TrashService::cancel() {
  g_cancellable_cancel(m_state->cancellable.get());
  m_state->prepared.reset();
  if (!busy() && m_state->snapshot.status != TrashStatus::Finished) {
    m_state->snapshot.status = TrashStatus::Cancelled;
  }
}
bool files::TrashService::prepare(const Location& parent, std::vector<Location> sources) {
  if (busy() || sources.empty()) {
    return false;
  }
  m_state->prepared.reset();
  m_state->cancellable.reset(g_cancellable_new());
  m_state->completed.store(0);
  m_state->snapshot = {.status = TrashStatus::Preparing, .results = {}, .error = {}};
  for (const auto& source : sources) {
    m_state->snapshot.results.push_back({source, TrashOutcome::Cancelled, {}});
  }
  State::launch(m_state, [owner = m_state, parent, sources = std::move(sources)] {
    const auto path = parent.nativePath();
    if (!path) {
      throw std::runtime_error("Moving remote items to Trash is not supported yet.");
    }
    auto prepared = std::make_shared<Prepared>(*path);
    std::set<std::string> names;
    for (const auto& source : sources) {
      if (g_cancellable_is_cancelled(owner->cancellable.get())) {
        return Reply{};
      }
      const auto native = source.nativePath();
      if (!native || std::filesystem::path(*native).parent_path().native() != *path) {
        throw std::runtime_error("Trash requires items from the current native folder.");
      }
      const auto name = std::filesystem::path(*native).filename().native();
      if (name.empty() || name == "." || name == ".." || !names.insert(name).second) {
        throw std::runtime_error("The Trash selection contains an invalid or repeated item.");
      }
      prepared->targets.push_back(std::make_unique<Target>(source, name, prepared->parent.get()));
    }
    return Reply{.prepared = std::move(prepared), .results = {}, .error = {}, .finished = false};
  });
  return true;
}
bool files::TrashService::trash() {
  if (!canTrash()) {
    return false;
  }
  m_state->snapshot.status = TrashStatus::Trashing;
  State::launch(m_state, [owner = m_state, prepared = m_state->prepared] {
    Reply reply{.prepared = {}, .results = {}, .error = {}, .finished = true};
    for (const auto& target : prepared->targets) {
      reply.results.push_back({target->location, TrashOutcome::Cancelled, {}});
    }
    for (std::size_t index = 0; index < prepared->targets.size(); ++index) {
      if (g_cancellable_is_cancelled(owner->cancellable.get())) {
        break;
      }
      const auto& target = *prepared->targets[index];
      auto& result = reply.results[index];
      try {
        Fd currentParent(::open(prepared->path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!sameIdentity(metadata(prepared->parent.get()), metadata(currentParent.get()))) {
          throw std::runtime_error("The containing folder changed. Select the items again.");
        }
        struct stat current{};
        if (::fstatat(prepared->parent.get(), target.name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0) {
          throw std::runtime_error(std::strerror(errno));
        }
        if (!sameIdentity(metadata(target.item.get()), current)) {
          throw std::runtime_error("The item was replaced or moved. Select it again.");
        }
        if (g_cancellable_is_cancelled(owner->cancellable.get())) {
          break;
        }
        GError* error = nullptr;
        if (g_file_trash(target.location.file().get(), owner->cancellable.get(), &error)) {
          result.outcome = TrashOutcome::Trashed;
        } else {
          result.outcome =
              g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ? TrashOutcome::Cancelled : TrashOutcome::Failed;
          result.error = error != nullptr ? error->message : "Could not move the item to Trash.";
        }
        g_clear_error(&error);
      } catch (const std::exception& error) {
        result.outcome = TrashOutcome::Failed;
        result.error = error.what();
      }
      owner->completed.fetch_add(1);
    }
    return reply;
  });
  return true;
}
