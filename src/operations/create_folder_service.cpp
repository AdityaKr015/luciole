#include "operations/create_folder_service.h"

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
    ~Fd() { ::close(m_fd); }
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
        throw std::runtime_error("The destination folder changed. Reopen New folder.");
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

struct files::CreateFolderService::State {
  CreateFolderSnapshot snapshot;
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
            owner->snapshot.status = CreateFolderStatus::SUCCEEDED;
            owner->target.reset();
          } else if (owner->cancelled.load()) {
            owner->snapshot.status = CreateFolderStatus::CANCELLED;
            owner->target.reset();
          } else if (!reply->error.empty()) {
            owner->snapshot.status = CreateFolderStatus::ERROR;
          } else {
            owner->target = std::move(reply->target);
            owner->snapshot.status = CreateFolderStatus::READY;
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
files::CreateFolderService::CreateFolderService() : m_state(std::make_shared<State>()) {}
files::CreateFolderService::~CreateFolderService() {
  m_state->changed = {};
  cancel();
}
void files::CreateFolderService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
bool files::CreateFolderService::busy() const { return m_state->active; }
bool files::CreateFolderService::canCreate() const { return !busy() && m_state->target != nullptr; }
const files::CreateFolderSnapshot& files::CreateFolderService::snapshot() const { return m_state->snapshot; }
void files::CreateFolderService::cancel() {
  m_state->cancelled.store(true);
  m_state->target.reset();
  if (!busy() && m_state->snapshot.status != CreateFolderStatus::SUCCEEDED)
    m_state->snapshot.status = CreateFolderStatus::CANCELLED;
}
bool files::CreateFolderService::prepare(const Location& parent) {
  if (busy())
    return false;
  m_state->target.reset();
  m_state->snapshot = {
      .status = CreateFolderStatus::PREPARING, .parent = parent, .destination = std::nullopt, .error = {}
  };
  State::launch(m_state, [parent] {
    const auto path = parent.nativePath();
    if (!path)
      throw std::runtime_error("Creating remote folders is not supported yet.");
    return Reply{.target = std::make_shared<Target>(*path), .destination = std::nullopt, .error = {}};
  });
  return true;
}
bool files::CreateFolderService::create(std::string name) {
  if (!canCreate())
    return false;
  if (const auto error = filenameProblem(name)) {
    m_state->snapshot.status = CreateFolderStatus::ERROR;
    m_state->snapshot.error = *error;
    m_state->notify();
    return false;
  }
  m_state->snapshot.status = CreateFolderStatus::CREATING;
  m_state->snapshot.error.clear();
  State::launch(m_state, [owner = m_state, target = m_state->target, name = std::move(name)] {
    auto destination = Location::fromPath((std::filesystem::path(target->path) / name).native());
    target->requireCurrent();
    const auto limit = ::fpathconf(target->directory.get(), _PC_NAME_MAX);
    if (limit >= 0 && name.size() > static_cast<std::size_t>(limit))
      throw std::runtime_error(creationError(ENAMETOOLONG));
    if (owner->cancelled.load())
      return Reply{};
    // mkdirat refuses every existing entry atomically and honors umask/default ACLs.
    if (::mkdirat(target->directory.get(), name.c_str(), 0777) != 0)
      throw std::runtime_error(creationError(errno));
    return Reply{.target = {}, .destination = std::move(destination), .error = {}};
  });
  return true;
}
