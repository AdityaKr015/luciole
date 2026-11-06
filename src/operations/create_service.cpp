#include "operations/create_service.h"

#include "fs/filename.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <linux/fs.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/syscall.h>
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
    ~Fd() {
      if (m_fd >= 0) {
        ::close(m_fd);
      }
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
    if (::fstat(fd, &value) != 0) {
      throw std::runtime_error(std::strerror(errno));
    }
    return value;
  }
  bool sameIdentity(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && (a.st_mode & S_IFMT) == (b.st_mode & S_IFMT);
  }
  struct TemplateSource {
    files::DocumentTemplate source;
    Fd file;
    explicit TemplateSource(files::DocumentTemplate source)
        : source(std::move(source)), file(
                                         ::open(
                                             this->source.location.nativePath().value_or("").c_str(),
                                             O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK
                                         )
                                     ) {
      requireCurrent();
    }
    void requireCurrent() const {
      Fd current(
          ::open(source.location.nativePath().value_or("").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)
      );
      if (files::templateRevision(metadata(current.get())) != source.revision
          || files::templateRevision(metadata(file.get())) != source.revision) {
        throw std::runtime_error("The template changed. Close this dialog and choose it again.");
      }
    }
  };
  struct Target {
    std::string path;
    Fd directory;
    std::unique_ptr<TemplateSource> source;
    explicit Target(std::string path)
        : path(std::move(path)), directory(::open(this->path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {}
    void requireCurrent() const {
      Fd current(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      if (!sameIdentity(metadata(current.get()), metadata(directory.get()))) {
        throw std::runtime_error("The destination folder changed. Close this dialog and try again.");
      }
    }
  };
  struct LinkSource {
    std::string path, name;
    Target parent;
    Fd item;
    explicit LinkSource(const files::DirectoryEntry& source)
        : path(source.location.nativePath().value_or("")), name(std::filesystem::path(path).filename().native()),
          parent(std::filesystem::path(path).parent_path().native()),
          item(::openat(parent.directory.get(), name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC)) {
      const files::GioPtr<GFileInfo> info(g_file_query_info(
          source.location.file().get(), "id::file,id::filesystem,standard::type,standard::is-symlink",
          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
      ));
      if (!info
          || files::readFileIdentity(info.get()) != source.identity
          || g_file_info_get_file_type(info.get()) != source.type
          || (g_file_info_get_is_symlink(info.get()) != FALSE) != source.symlink) {
        throw std::runtime_error("The selected item changed. Choose it again.");
      }
      requireCurrent();
    }
    void requireCurrent() const {
      parent.requireCurrent();
      struct stat current{};
      if (::fstatat(parent.directory.get(), name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0
          || !sameIdentity(current, metadata(item.get()))) {
        throw std::runtime_error("The source was moved or replaced. Close this dialog and choose it again.");
      }
    }
  };
  struct Reply {
    std::shared_ptr<Target> target;
    std::optional<files::Location> destination;
    std::string error;
    std::shared_ptr<LinkSource> linkSource;
  };
  std::string creationError(int code) {
    if (code == EEXIST || code == ENOTEMPTY) {
      return "An item with that name already exists.";
    }
    if (code == ENAMETOOLONG) {
      return "The name is too long for this folder.";
    }
    if (code == EACCES || code == EPERM) {
      return "Permission denied.";
    }
    if (code == ENOENT) {
      return "The destination folder is no longer available.";
    }
    if (code == EXDEV) {
      return "Hard links require a destination on the same filesystem.";
    }
    return std::strerror(code);
  }
  // Private staging keeps incomplete template contents out of the requested destination name.
  class DocumentStage {
  public:
    explicit DocumentStage(int parent) : m_parent(parent) {
      const files::GString uuid(g_uuid_string_random());
      m_name = ".noctalia-document-" + std::string(uuid.get());
      if (::mkdirat(parent, m_name.c_str(), 0700) != 0) {
        throw std::runtime_error(creationError(errno));
      }
      const int fd = ::openat(parent, m_name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0) {
        const int code = errno;
        ::unlinkat(parent, m_name.c_str(), AT_REMOVEDIR);
        throw std::runtime_error(creationError(code));
      }
      m_directory = std::make_unique<Fd>(fd);
    }
    ~DocumentStage() { (void)clear(); }
    DocumentStage(const DocumentStage&) = delete;
    DocumentStage& operator=(const DocumentStage&) = delete;
    int fd() const { return m_directory->get(); }
    bool clear() const {
      struct stat current{}, held{};
      if (::fstat(fd(), &held) != 0 || ::fstatat(m_parent, m_name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT;
      }
      if (!sameIdentity(current, held)) {
        return false;
      }
      if (::unlinkat(fd(), "document", 0) != 0 && errno != ENOENT) {
        return false;
      }
      return ::unlinkat(m_parent, m_name.c_str(), AT_REMOVEDIR) == 0;
    }

  private:
    int m_parent;
    std::string m_name;
    std::unique_ptr<Fd> m_directory;
  };
  std::optional<bool> copyDocument(const Target& target, const std::string& name, const std::atomic<bool>& cancelled) {
    auto& source = *target.source;
    source.requireCurrent();
    DocumentStage stage(target.directory.get());
    Fd output(::openat(stage.fd(), "document", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666));
    std::array<char, 65536> buffer{};
    std::uint64_t offset = 0;
    while (offset < source.source.revision.size) {
      if (cancelled.load()) {
        return std::nullopt;
      }
      const auto count = ::pread(
          source.file.get(), buffer.data(),
          std::min<std::uint64_t>(buffer.size(), source.source.revision.size - offset), static_cast<off_t>(offset)
      );
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        throw std::runtime_error(count == 0 ? "The template changed while copying." : creationError(errno));
      }
      std::size_t written = 0;
      while (written < static_cast<std::size_t>(count)) {
        if (cancelled.load()) {
          return std::nullopt;
        }
        const auto size = ::write(output.get(), buffer.data() + written, static_cast<std::size_t>(count) - written);
        if (size < 0 && errno == EINTR) {
          continue;
        }
        if (size <= 0) {
          throw std::runtime_error(size == 0 ? "Could not write the new document." : creationError(errno));
        }
        written += static_cast<std::size_t>(size);
      }
      offset += static_cast<std::uint64_t>(count);
    }
    if (output.close() != 0) {
      throw std::runtime_error("Could not close the new document cleanly.");
    }
    source.requireCurrent();
    target.requireCurrent();
    if (cancelled.load()) {
      return std::nullopt;
    }
    if (::syscall(SYS_renameat2, stage.fd(), "document", target.directory.get(), name.c_str(), RENAME_NOREPLACE) != 0) {
      throw std::runtime_error(creationError(errno));
    }
    // Publication is complete; cancellation cannot turn it into an uncommitted result.
    return stage.clear();
  }
} // namespace

struct files::CreateService::State {
  CreateSnapshot snapshot;
  std::shared_ptr<Target> target;
  std::shared_ptr<LinkSource> linkSource;
  std::atomic<bool> cancelled{false};
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
            owner->snapshot.status = reply->error.empty() ? CreateStatus::Succeeded : CreateStatus::Partial;
            owner->target.reset();
            owner->linkSource.reset();
          } else if (owner->cancelled.load()) {
            owner->snapshot.status = CreateStatus::Cancelled;
            owner->target.reset();
            owner->linkSource.reset();
          } else if (!reply->error.empty()) {
            owner->snapshot.status = CreateStatus::Error;
          } else {
            owner->target = std::move(reply->target);
            owner->linkSource = std::move(reply->linkSource);
            owner->snapshot.status = CreateStatus::Ready;
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
        if (!request.owner->cancelled.load()) {
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
files::CreateService::CreateService() : m_state(std::make_shared<State>()) {}
files::CreateService::~CreateService() {
  m_state->changed = {};
  cancel();
}
void files::CreateService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
bool files::CreateService::busy() const { return m_state->active; }
bool files::CreateService::canCreate() const {
  return !busy() && (m_state->target != nullptr || m_state->linkSource != nullptr);
}
const files::CreateSnapshot& files::CreateService::snapshot() const { return m_state->snapshot; }
void files::CreateService::cancel() {
  m_state->cancelled.store(true);
  m_state->target.reset();
  m_state->linkSource.reset();
  if (!busy()
      && m_state->snapshot.status != CreateStatus::Succeeded
      && m_state->snapshot.status != CreateStatus::Partial) {
    m_state->snapshot.status = CreateStatus::Cancelled;
  }
}
bool files::CreateService::prepare(const Location& parent, CreateKind kind) {
  if (kind != CreateKind::Folder && kind != CreateKind::EmptyFile) {
    return false;
  }
  return prepareTarget(parent, kind, std::nullopt);
}
bool files::CreateService::prepareTemplate(const Location& parent, DocumentTemplate source) {
  return prepareTarget(parent, CreateKind::Document, std::move(source));
}
bool files::CreateService::prepareLink(const DirectoryEntry& source) {
  if (busy()
      || !source.location.nativePath()
      || !source.identity
      || (source.type != G_FILE_TYPE_REGULAR
          && source.type != G_FILE_TYPE_DIRECTORY
          && source.type != G_FILE_TYPE_SYMBOLIC_LINK)) {
    return false;
  }
  m_state->target.reset();
  m_state->linkSource.reset();
  m_state->snapshot = {
      .status = CreateStatus::Preparing,
      .kind = CreateKind::SymbolicLink,
      .parent = std::nullopt,
      .destination = std::nullopt,
      .error = {}
  };
  State::launch(m_state, [source] {
    return Reply{
        .target = {}, .destination = std::nullopt, .error = {}, .linkSource = std::make_shared<LinkSource>(source)
    };
  });
  return true;
}
bool files::CreateService::createLink(const Location& parent, std::string name, CreateKind kind) {
  if (!canCreate() || !m_state->linkSource || (kind != CreateKind::SymbolicLink && kind != CreateKind::HardLink)) {
    return false;
  }
  if (const auto error = filenameProblem(name)) {
    m_state->snapshot.status = CreateStatus::Error;
    m_state->snapshot.error = *error;
    m_state->notify();
    return false;
  }
  m_state->snapshot.kind = kind;
  m_state->snapshot.parent = parent;
  m_state->snapshot.status = CreateStatus::Creating;
  m_state->snapshot.error.clear();
  State::launch(m_state, [owner = m_state, source = m_state->linkSource, parent, name = std::move(name), kind] {
    const auto path = parent.nativePath();
    if (!path) {
      throw std::runtime_error("Choose a native destination folder.");
    }
    Target destination(*path);
    destination.requireCurrent();
    source->requireCurrent();
    const auto sourceInfo = metadata(source->item.get());
    if (kind == CreateKind::HardLink && !S_ISREG(sourceInfo.st_mode)) {
      throw std::runtime_error("Hard links are available only for regular files, without following symbolic links.");
    }
    if (kind == CreateKind::HardLink && sourceInfo.st_dev != metadata(destination.directory.get()).st_dev) {
      throw std::runtime_error(creationError(EXDEV));
    }
    const auto limit = ::fpathconf(destination.directory.get(), _PC_NAME_MAX);
    if (limit >= 0 && name.size() > static_cast<std::size_t>(limit)) {
      throw std::runtime_error(creationError(ENAMETOOLONG));
    }
    Reply reply{
        .target = {},
        .destination = Location::fromPath((std::filesystem::path(*path) / name).native()),
        .error = {},
        .linkSource = {}
    };
    destination.requireCurrent();
    if (owner->cancelled.load()) {
      return Reply{};
    }
    const auto held = "/proc/self/fd/" + std::to_string(source->item.get());
    const int result = kind == CreateKind::HardLink
        ? ::linkat(AT_FDCWD, held.c_str(), destination.directory.get(), name.c_str(), AT_SYMLINK_FOLLOW)
        : ::symlinkat(source->path.c_str(), destination.directory.get(), name.c_str());
    if (result != 0) {
      throw std::runtime_error(creationError(errno));
    }
    return reply;
  });
  return true;
}
bool files::CreateService::prepareTarget(
    const Location& parent, CreateKind kind, std::optional<DocumentTemplate> source
) {
  if (busy()) {
    return false;
  }
  m_state->target.reset();
  m_state->linkSource.reset();
  m_state->snapshot = {
      .status = CreateStatus::Preparing, .kind = kind, .parent = parent, .destination = std::nullopt, .error = {}
  };
  State::launch(m_state, [parent, source = std::move(source)] mutable {
    const auto path = parent.nativePath();
    if (!path) {
      throw std::runtime_error("Creating remote files or folders is not supported yet.");
    }
    auto target = std::make_shared<Target>(*path);
    if (source) {
      target->source = std::make_unique<TemplateSource>(std::move(*source));
    }
    return Reply{.target = std::move(target), .destination = std::nullopt, .error = {}, .linkSource = {}};
  });
  return true;
}
bool files::CreateService::create(std::string name) {
  if (!canCreate() || !m_state->target) {
    return false;
  }
  if (const auto error = filenameProblem(name)) {
    m_state->snapshot.status = CreateStatus::Error;
    m_state->snapshot.error = *error;
    m_state->notify();
    return false;
  }
  m_state->snapshot.status = CreateStatus::Creating;
  m_state->snapshot.error.clear();
  State::launch(
      m_state, [owner = m_state, target = m_state->target, kind = m_state->snapshot.kind, name = std::move(name)] {
        Reply result{
            .target = {},
            .destination = Location::fromPath((std::filesystem::path(target->path) / name).native()),
            .error = {},
            .linkSource = {}
        };
        std::string closeError = "The file was created, but it could not be closed cleanly.";
        target->requireCurrent();
        const auto limit = ::fpathconf(target->directory.get(), _PC_NAME_MAX);
        if (limit >= 0 && name.size() > static_cast<std::size_t>(limit)) {
          throw std::runtime_error(creationError(ENAMETOOLONG));
        }
        if (owner->cancelled.load()) {
          return Reply{};
        }
        if (kind == CreateKind::Folder) {
          if (::mkdirat(target->directory.get(), name.c_str(), 0777) != 0) {
            throw std::runtime_error(creationError(errno));
          }
        } else if (kind == CreateKind::Document) {
          const auto clean = copyDocument(*target, name, owner->cancelled);
          if (!clean) {
            return Reply{};
          }
          if (!*clean) {
            result.error = "The document was created, but its temporary folder could not be removed.";
          }
        } else {
          // Exclusive creation refuses every existing entry, including dangling links and FIFOs.
          const int fd = ::openat(
              target->directory.get(), name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666
          );
          if (fd < 0) {
            throw std::runtime_error(creationError(errno));
          }
          Fd output(fd);
          // Linux closes the descriptor even on EINTR; retrying could close an unrelated descriptor.
          if (output.close() != 0) {
            result.error = std::move(closeError);
          }
        }
        return result;
      }
  );
  return true;
}
