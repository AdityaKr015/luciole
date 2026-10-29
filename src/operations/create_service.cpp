#include "operations/create_service.h"

#include "fs/filename.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
  class Fd {
  public:
    explicit Fd(int fd) : m_fd(fd) {
      if (fd < 0)
        throw std::runtime_error(std::strerror(errno));
    }
    ~Fd() {
      if (m_fd >= 0)
        ::close(m_fd);
    }
    int close() { return ::close(std::exchange(m_fd, -1)); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return m_fd; }

  private:
    int m_fd;
  };
  struct stat metadata(int fd) {
    struct stat value{};
    if (::fstat(fd, &value) != 0)
      throw std::runtime_error(std::strerror(errno));
    return value;
  }
  bool sameIdentity(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && (a.st_mode & S_IFMT) == (b.st_mode & S_IFMT);
  }
  struct Target {
    std::string path;
    Fd directory;
    explicit Target(std::string path)
        : path(std::move(path)), directory(::open(this->path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {}
    void requireCurrent() const {
      Fd current(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      if (!sameIdentity(metadata(current.get()), metadata(directory.get())))
        throw std::runtime_error("The destination folder changed. Close this dialog and try again.");
    }
  };
  struct Reply {
    std::shared_ptr<Target> target;
    std::optional<files::Location> destination;
    std::string error;
  };
  std::string creationError(int code) {
    if (code == EEXIST || code == ENOTEMPTY)
      return "An item with that name already exists.";
    if (code == ENAMETOOLONG)
      return "The name is too long for this folder.";
    if (code == EACCES || code == EPERM)
      return "Permission denied.";
    if (code == ENOENT)
      return "The destination folder is no longer available.";
    return std::strerror(code);
  }
} // namespace

struct files::CreateService::State {
  CreateSnapshot snapshot;
  std::shared_ptr<Target> target;
  std::atomic<bool> cancelled{false};
  bool active = false;
  std::function<void()> changed;
  void notify() const {
    const auto callback = changed;
    if (callback)
      callback();
  }
  static void launch(const std::shared_ptr<State>& owner, std::function<Reply()> work) {
    struct Request {
      std::shared_ptr<State> owner;
      std::function<Reply()> work;
    };
    owner->active = true;
    owner->cancelled.store(false);
    GioPtr<GTask> task(g_task_new(
        nullptr, nullptr,
        [](GObject*, GAsyncResult* result, gpointer) {
          auto* task = G_TASK(result);
          const auto owner = static_cast<Request*>(g_task_get_task_data(task))->owner;
          std::unique_ptr<Reply> reply(static_cast<Reply*>(g_task_propagate_pointer(task, nullptr)));
          owner->active = false;
          owner->snapshot.error = reply->error;
          if (reply->destination) {
            owner->snapshot.destination = std::move(reply->destination);
            owner->snapshot.status = reply->error.empty() ? CreateStatus::SUCCEEDED : CreateStatus::PARTIAL;
            owner->target.reset();
          } else if (owner->cancelled.load()) {
            owner->snapshot.status = CreateStatus::CANCELLED;
            owner->target.reset();
          } else if (!reply->error.empty()) {
            owner->snapshot.status = CreateStatus::ERROR;
          } else {
            owner->target = std::move(reply->target);
            owner->snapshot.status = CreateStatus::READY;
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
        if (!request.owner->cancelled.load())
          *reply = request.work();
      } catch (const std::exception& error) {
        reply->error = error.what();
      }
      g_task_return_pointer(task, reply.release(), [](gpointer data) { delete static_cast<Reply*>(data); });
    });
    owner->notify();
  }
};
files::CreateService::CreateService() : m_state(std::make_shared<State>()) {}
files::CreateService::~CreateService() {
  m_state->changed = {};
  cancel();
}
void files::CreateService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
bool files::CreateService::busy() const { return m_state->active; }
bool files::CreateService::canCreate() const { return !busy() && m_state->target != nullptr; }
const files::CreateSnapshot& files::CreateService::snapshot() const { return m_state->snapshot; }
void files::CreateService::cancel() {
  m_state->cancelled.store(true);
  m_state->target.reset();
  if (!busy()
      && m_state->snapshot.status != CreateStatus::SUCCEEDED
      && m_state->snapshot.status != CreateStatus::PARTIAL)
    m_state->snapshot.status = CreateStatus::CANCELLED;
}
bool files::CreateService::prepare(const Location& parent, CreateKind kind) {
  if (busy() || (kind != CreateKind::FOLDER && kind != CreateKind::EMPTY_FILE))
    return false;
  m_state->target.reset();
  m_state->snapshot = {
      .status = CreateStatus::PREPARING, .kind = kind, .parent = parent, .destination = std::nullopt, .error = {}
  };
  State::launch(m_state, [parent] {
    const auto path = parent.nativePath();
    if (!path)
      throw std::runtime_error("Creating remote files or folders is not supported yet.");
    return Reply{.target = std::make_shared<Target>(*path), .destination = std::nullopt, .error = {}};
  });
  return true;
}
bool files::CreateService::create(std::string name) {
  if (!canCreate())
    return false;
  if (const auto error = filenameProblem(name)) {
    m_state->snapshot.status = CreateStatus::ERROR;
    m_state->snapshot.error = *error;
    m_state->notify();
    return false;
  }
  m_state->snapshot.status = CreateStatus::CREATING;
  m_state->snapshot.error.clear();
  State::launch(
      m_state, [owner = m_state, target = m_state->target, kind = m_state->snapshot.kind, name = std::move(name)] {
        Reply result{
            .target = {},
            .destination = Location::fromPath((std::filesystem::path(target->path) / name).native()),
            .error = {}
        };
        std::string closeError = "The file was created, but it could not be closed cleanly.";
        target->requireCurrent();
        const auto limit = ::fpathconf(target->directory.get(), _PC_NAME_MAX);
        if (limit >= 0 && name.size() > static_cast<std::size_t>(limit))
          throw std::runtime_error(creationError(ENAMETOOLONG));
        if (owner->cancelled.load())
          return Reply{};
        if (kind == CreateKind::FOLDER) {
          if (::mkdirat(target->directory.get(), name.c_str(), 0777) != 0)
            throw std::runtime_error(creationError(errno));
        } else {
          // Exclusive creation refuses every existing entry, including dangling links and FIFOs.
          const int fd = ::openat(
              target->directory.get(), name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666
          );
          if (fd < 0)
            throw std::runtime_error(creationError(errno));
          Fd output(fd);
          // Linux closes the descriptor even on EINTR; retrying could close an unrelated descriptor.
          if (output.close() != 0)
            result.error = std::move(closeError);
        }
        return result;
      }
  );
  return true;
}
