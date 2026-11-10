#include "operations/create_service.h"

#include "fs/background_job.h"
#include "fs/filename.h"
#include "fs/gio_interop.h"
#include "fs/posix_fd.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace {
  struct TemplateSource {
    luciole::DocumentTemplate source;
    luciole::Fd file;
    explicit TemplateSource(luciole::DocumentTemplate source)
        : source(std::move(source)), file(
                                         luciole::checkedFd(
                                             ::open(
                                                 this->source.location.nativePath().value_or("").c_str(),
                                                 O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK
                                             )
                                         )
                                     ) {
      requireCurrent();
    }
    void requireCurrent() const {
      const auto current = luciole::checkedFd(
          ::open(source.location.nativePath().value_or("").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)
      );
      if (luciole::templateRevision(luciole::statFd(current.get())) != source.revision
          || luciole::templateRevision(luciole::statFd(file.get())) != source.revision) {
        luciole::throwFileError(
            luciole::FileErrorKind::Changed, "The template changed. Close this dialog and choose it again."
        );
      }
    }
  };
  struct Target {
    std::string path;
    luciole::Fd directory;
    std::unique_ptr<TemplateSource> source;
    explicit Target(std::string path)
        : path(std::move(path)),
          directory(luciole::checkedFd(::open(this->path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC))) {}
    void requireCurrent() const {
      const auto current = luciole::checkedFd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      if (!luciole::sameIdentity(luciole::statFd(current.get()), luciole::statFd(directory.get()))) {
        luciole::throwFileError(
            luciole::FileErrorKind::Changed, "The destination folder changed. Close this dialog and try again."
        );
      }
    }
  };
  struct LinkSource {
    std::string path, name;
    Target parent;
    luciole::Fd item;
    explicit LinkSource(const luciole::DirectoryEntry& source)
        : path(source.location.nativePath().value_or("")), name(std::filesystem::path(path).filename().native()),
          parent(std::filesystem::path(path).parent_path().native()),
          item(luciole::checkedFd(::openat(parent.directory.get(), name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC))) {
      const luciole::GioPtr<GFileInfo> info(g_file_query_info(
          luciole::gioFile(source.location).get(), "id::file,id::filesystem,standard::type,standard::is-symlink",
          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
      ));
      if (!info
          || luciole::readFileIdentity(info.get()) != source.identity
          || luciole::fileType(g_file_info_get_file_type(info.get())) != source.type
          || (g_file_info_get_is_symlink(info.get()) != FALSE) != source.symlink) {
        luciole::throwFileError(luciole::FileErrorKind::Changed, "The selected item changed. Choose it again.");
      }
      requireCurrent();
    }
    void requireCurrent() const {
      parent.requireCurrent();
      struct stat current{};
      if (::fstatat(parent.directory.get(), name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0
          || !luciole::sameIdentity(current, luciole::statFd(item.get()))) {
        luciole::throwFileError(
            luciole::FileErrorKind::Changed, "The source was moved or replaced. Close this dialog and choose it again."
        );
      }
    }
  };
  struct Reply {
    std::shared_ptr<Target> target;
    std::optional<luciole::Location> destination;
    // A completed creation whose cleanup or close failed.
    std::optional<luciole::FileError> error;
    std::shared_ptr<LinkSource> linkSource;
  };
  constexpr auto crossDeviceLink = "Hard links require a destination on the same filesystem.";
  // Private staging keeps incomplete template contents out of the requested destination name.
  class DocumentStage {
  public:
    explicit DocumentStage(int parent) : m_parent(parent) {
      const luciole::GString uuid(g_uuid_string_random());
      m_name = ".luciole-document-" + std::string(uuid.get());
      if (::mkdirat(parent, m_name.c_str(), 0700) != 0) {
        luciole::throwErrno(errno);
      }
      m_directory = luciole::Fd(::openat(parent, m_name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (!m_directory.valid()) {
        const int code = errno;
        ::unlinkat(parent, m_name.c_str(), AT_REMOVEDIR);
        luciole::throwErrno(code);
      }
    }
    ~DocumentStage() { (void)clear(); }
    DocumentStage(const DocumentStage&) = delete;
    DocumentStage& operator=(const DocumentStage&) = delete;
    int fd() const { return m_directory.get(); }
    bool clear() const {
      struct stat current{}, held{};
      if (::fstat(fd(), &held) != 0 || ::fstatat(m_parent, m_name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT;
      }
      if (!luciole::sameIdentity(current, held)) {
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
    luciole::Fd m_directory;
  };
  std::optional<bool> copyDocument(const Target& target, const std::string& name, const luciole::CancelToken& token) {
    auto& source = *target.source;
    source.requireCurrent();
    DocumentStage stage(target.directory.get());
    auto output = luciole::checkedFd(
        ::openat(stage.fd(), "document", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666)
    );
    std::array<char, 65536> buffer{};
    std::uint64_t offset = 0;
    while (offset < source.source.revision.size) {
      if (token.cancelled()) {
        return std::nullopt;
      }
      const auto count = ::pread(
          source.file.get(), buffer.data(),
          std::min<std::uint64_t>(buffer.size(), source.source.revision.size - offset), static_cast<off_t>(offset)
      );
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count == 0) {
        luciole::throwFileError(luciole::FileErrorKind::Changed, "The template changed while copying.");
      }
      if (count < 0) {
        luciole::throwErrno(errno);
      }
      std::size_t written = 0;
      while (written < static_cast<std::size_t>(count)) {
        if (token.cancelled()) {
          return std::nullopt;
        }
        const auto size = ::write(output.get(), buffer.data() + written, static_cast<std::size_t>(count) - written);
        if (size < 0 && errno == EINTR) {
          continue;
        }
        if (size == 0) {
          luciole::throwFileError(luciole::FileErrorKind::Other, "Could not write the new document.");
        }
        if (size < 0) {
          luciole::throwErrno(errno);
        }
        written += static_cast<std::size_t>(size);
      }
      offset += static_cast<std::uint64_t>(count);
    }
    if (output.close() != 0) {
      luciole::throwFileError(luciole::FileErrorKind::Other, "Could not close the new document cleanly.");
    }
    source.requireCurrent();
    target.requireCurrent();
    if (token.cancelled()) {
      return std::nullopt;
    }
    if (::syscall(SYS_renameat2, stage.fd(), "document", target.directory.get(), name.c_str(), RENAME_NOREPLACE) != 0) {
      luciole::throwErrno(errno);
    }
    // Publication is complete; cancellation cannot turn it into an uncommitted result.
    return stage.clear();
  }
} // namespace

struct luciole::CreateService::State {
  CreateSnapshot snapshot;
  std::shared_ptr<Target> target;
  std::shared_ptr<LinkSource> linkSource;
  CancelToken token;
  bool active = false;
  std::function<void()> changed;
  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  static void launch(const std::shared_ptr<State>& owner, std::move_only_function<Reply(const CancelToken&)> work) {
    owner->active = true;
    owner->token = CancelToken{};
    runJob<Reply>(
        owner->token, [token = owner->token, work = std::move(work)] mutable { return work(token); },
        [owner, token = owner->token](JobResult<Reply> result) {
          owner->active = false;
          owner->snapshot.error.reset();
          if (result) {
            owner->snapshot.error = std::move(result->error);
          } else if (result.error().kind != FileErrorKind::Cancelled) {
            owner->snapshot.error = std::move(result.error());
          }
          if (result && result->destination) {
            owner->snapshot.destination = std::move(result->destination);
            owner->snapshot.status = owner->snapshot.error ? CreateStatus::Partial : CreateStatus::Succeeded;
            owner->target.reset();
            owner->linkSource.reset();
          } else if (token.cancelled()) {
            owner->snapshot.status = CreateStatus::Cancelled;
            owner->target.reset();
            owner->linkSource.reset();
          } else if (!result) {
            owner->snapshot.status = CreateStatus::Error;
          } else {
            owner->target = std::move(result->target);
            owner->linkSource = std::move(result->linkSource);
            owner->snapshot.status = CreateStatus::Ready;
          }
          owner->notify();
        }
    );
    owner->notify();
  }
};
luciole::CreateService::CreateService() : m_state(std::make_shared<State>()) {}
luciole::CreateService::~CreateService() {
  m_state->changed = {};
  cancel();
}
void luciole::CreateService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
bool luciole::CreateService::busy() const { return m_state->active; }
bool luciole::CreateService::canCreate() const {
  return !busy() && (m_state->target != nullptr || m_state->linkSource != nullptr);
}
const luciole::CreateSnapshot& luciole::CreateService::snapshot() const { return m_state->snapshot; }
void luciole::CreateService::cancel() {
  m_state->token.cancel();
  m_state->target.reset();
  m_state->linkSource.reset();
  if (!busy()
      && m_state->snapshot.status != CreateStatus::Succeeded
      && m_state->snapshot.status != CreateStatus::Partial) {
    m_state->snapshot.status = CreateStatus::Cancelled;
  }
}
bool luciole::CreateService::prepare(const Location& parent, CreateKind kind) {
  if (kind != CreateKind::Folder && kind != CreateKind::EmptyFile) {
    return false;
  }
  return prepareTarget(parent, kind, std::nullopt);
}
bool luciole::CreateService::prepareTemplate(const Location& parent, DocumentTemplate source) {
  return prepareTarget(parent, CreateKind::Document, std::move(source));
}
bool luciole::CreateService::prepareLink(const DirectoryEntry& source) {
  if (busy()
      || !source.location.nativePath()
      || !source.identity
      || (source.type != FileType::Regular && source.type != FileType::Directory && source.type != FileType::Symlink)) {
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
  State::launch(m_state, [source](const CancelToken&) {
    return Reply{
        .target = {}, .destination = std::nullopt, .error = {}, .linkSource = std::make_shared<LinkSource>(source)
    };
  });
  return true;
}
bool luciole::CreateService::createLink(const Location& parent, std::string name, CreateKind kind) {
  if (!canCreate() || !m_state->linkSource || (kind != CreateKind::SymbolicLink && kind != CreateKind::HardLink)) {
    return false;
  }
  if (const auto error = filenameProblem(name)) {
    m_state->snapshot.status = CreateStatus::Error;
    m_state->snapshot.error = FileError{.kind = FileErrorKind::InvalidArgument, .message = *error};
    m_state->notify();
    return false;
  }
  m_state->snapshot.kind = kind;
  m_state->snapshot.parent = parent;
  m_state->snapshot.status = CreateStatus::Creating;
  m_state->snapshot.error.reset();
  State::launch(
      m_state, [source = m_state->linkSource, parent, name = std::move(name), kind](const CancelToken& token) {
        const auto path = parent.nativePath();
        if (!path) {
          throwFileError(FileErrorKind::Unsupported, "Choose a native destination folder.");
        }
        Target destination(*path);
        destination.requireCurrent();
        source->requireCurrent();
        const auto sourceInfo = statFd(source->item.get());
        if (kind == CreateKind::HardLink && !S_ISREG(sourceInfo.st_mode)) {
          throwFileError(
              FileErrorKind::Unsupported,
              "Hard links are available only for regular files, without following symbolic links."
          );
        }
        if (kind == CreateKind::HardLink && sourceInfo.st_dev != statFd(destination.directory.get()).st_dev) {
          throwFileError(FileErrorKind::Unsupported, crossDeviceLink);
        }
        const auto limit = ::fpathconf(destination.directory.get(), _PC_NAME_MAX);
        if (limit >= 0 && name.size() > static_cast<std::size_t>(limit)) {
          throwErrno(ENAMETOOLONG);
        }
        Reply reply{
            .target = {},
            .destination = Location::fromPath((std::filesystem::path(*path) / name).native()),
            .error = {},
            .linkSource = {}
        };
        destination.requireCurrent();
        if (token.cancelled()) {
          return Reply{};
        }
        const auto held = "/proc/self/fd/" + std::to_string(source->item.get());
        const int result = kind == CreateKind::HardLink
            ? ::linkat(AT_FDCWD, held.c_str(), destination.directory.get(), name.c_str(), AT_SYMLINK_FOLLOW)
            : ::symlinkat(source->path.c_str(), destination.directory.get(), name.c_str());
        if (result != 0) {
          if (errno == EXDEV) {
            throwFileError(FileErrorKind::Unsupported, crossDeviceLink);
          }
          throwErrno(errno);
        }
        return reply;
      }
  );
  return true;
}
bool luciole::CreateService::prepareTarget(
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
  State::launch(m_state, [parent, source = std::move(source)](const CancelToken&) mutable {
    const auto path = parent.nativePath();
    if (!path) {
      throwFileError(FileErrorKind::Unsupported, "Creating remote files or folders is not supported yet.");
    }
    auto target = std::make_shared<Target>(*path);
    if (source) {
      target->source = std::make_unique<TemplateSource>(std::move(*source));
    }
    return Reply{.target = std::move(target), .destination = std::nullopt, .error = {}, .linkSource = {}};
  });
  return true;
}
bool luciole::CreateService::create(std::string name) {
  if (!canCreate() || !m_state->target) {
    return false;
  }
  if (const auto error = filenameProblem(name)) {
    m_state->snapshot.status = CreateStatus::Error;
    m_state->snapshot.error = FileError{.kind = FileErrorKind::InvalidArgument, .message = *error};
    m_state->notify();
    return false;
  }
  m_state->snapshot.status = CreateStatus::Creating;
  m_state->snapshot.error.reset();
  State::launch(
      m_state,
      [target = m_state->target, kind = m_state->snapshot.kind, name = std::move(name)](const CancelToken& token) {
        Reply result{
            .target = {},
            .destination = Location::fromPath((std::filesystem::path(target->path) / name).native()),
            .error = {},
            .linkSource = {}
        };
        target->requireCurrent();
        const auto limit = ::fpathconf(target->directory.get(), _PC_NAME_MAX);
        if (limit >= 0 && name.size() > static_cast<std::size_t>(limit)) {
          throwErrno(ENAMETOOLONG);
        }
        if (token.cancelled()) {
          return Reply{};
        }
        if (kind == CreateKind::Folder) {
          if (::mkdirat(target->directory.get(), name.c_str(), 0777) != 0) {
            throwErrno(errno);
          }
        } else if (kind == CreateKind::Document) {
          const auto clean = copyDocument(*target, name, token);
          if (!clean) {
            return Reply{};
          }
          if (!*clean) {
            result.error = FileError{
                .kind = FileErrorKind::Other,
                .message = "The document was created, but its temporary folder could not be removed."
            };
          }
        } else {
          // Exclusive creation refuses every existing entry, including dangling links and FIFOs.
          auto output = checkedFd(
              ::openat(
                  target->directory.get(), name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666
              )
          );
          // Linux closes the descriptor even on EINTR; retrying could close an unrelated descriptor.
          if (output.close() != 0) {
            result.error = FileError{
                .kind = FileErrorKind::Other, .message = "The file was created, but it could not be closed cleanly."
            };
          }
        }
        return result;
      }
  );
  return true;
}
