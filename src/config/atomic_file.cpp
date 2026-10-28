#include "config/atomic_file.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <format>
#include <linux/fs.h>
#include <string_view>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace {
  constexpr std::size_t kMaxBytes = 256 * 1024;

  struct Descriptor {
    int fd = -1;
    explicit Descriptor(int value = -1) : fd(value) {}
    ~Descriptor() { close(); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
      if (this != &other) {
        close();
        fd = std::exchange(other.fd, -1);
      }
      return *this;
    }
    int close() { return fd < 0 ? 0 : ::close(std::exchange(fd, -1)); }
  };

  std::string failure(std::string_view operation, int error = errno) {
    return std::format("{}: {}", operation, std::error_code(error, std::generic_category()).message());
  }

  bool sync(int fd) {
    while (::fsync(fd) < 0) {
      if (errno != EINTR) {
        return false;
      }
    }
    return true;
  }

  std::expected<Descriptor, std::string> openParent(const std::filesystem::path& path) {
    Descriptor parent(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (parent.fd < 0) {
      return std::unexpected(failure("open directory"));
    }
    for (const auto& part : path.relative_path()) {
      auto next = ::openat(parent.fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (next < 0 && errno == ENOENT) {
        const bool created = ::mkdirat(parent.fd, part.c_str(), 0700) == 0;
        if (!created && errno != EEXIST) {
          return std::unexpected(failure("create config directory"));
        }
        next = ::openat(parent.fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (next < 0) {
          return std::unexpected(failure("open config directory"));
        }
        Descriptor child(next);
        if (created && (!sync(child.fd) || !sync(parent.fd))) {
          return std::unexpected(failure("sync new config directory"));
        }
        parent = std::move(child);
      } else if (next < 0) {
        return std::unexpected(failure("open config directory"));
      } else {
        parent = Descriptor(next);
      }
    }
    return parent;
  }

  bool sameRevision(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev
        && a.st_ino == b.st_ino
        && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec
        && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec
        && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec
        && a.st_mode == b.st_mode
        && a.st_nlink == b.st_nlink
        && a.st_uid == b.st_uid;
  }

  struct CurrentFile {
    std::optional<std::string> content;
    struct stat revision{};
  };

  std::expected<CurrentFile, std::string> readCurrent(int parent, const std::string& name) {
    CurrentFile current;
    if (::fstatat(parent, name.c_str(), &current.revision, AT_SYMLINK_NOFOLLOW) < 0) {
      if (errno == ENOENT) {
        return current;
      }
      return std::unexpected(failure("inspect settings"));
    }
    if (!S_ISREG(current.revision.st_mode)
        || current.revision.st_nlink != 1
        || current.revision.st_uid != ::geteuid()
        || (current.revision.st_mode & S_IWUSR) == 0) {
      return std::unexpected("settings must be an owned, writable regular file with no hard links or symlink");
    }
    const Descriptor file(::openat(parent, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.fd < 0) {
      return std::unexpected(failure("read settings"));
    }
    struct stat before{};
    if (::fstat(file.fd, &before) < 0 || !sameRevision(before, current.revision)) {
      return std::unexpected("settings changed while opening; reload and retry");
    }
    current.content.emplace();
    std::array<char, 8192> buffer{};
    while (true) {
      const auto count = ::read(file.fd, buffer.data(), buffer.size());
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        return std::unexpected(failure("read settings"));
      }
      if (count == 0) {
        break;
      }
      const auto size = static_cast<std::size_t>(count);
      if (current.content->size() + size > kMaxBytes) {
        return std::unexpected("settings exceed the 256 KiB limit");
      }
      current.content->append(buffer.data(), size);
    }
    struct stat after{};
    if (::fstat(file.fd, &after) < 0 || !sameRevision(before, after)) {
      return std::unexpected("settings changed while reading; reload and retry");
    }
    return current;
  }

  bool stillCurrent(int parent, const std::string& name, const CurrentFile& expected) {
    struct stat actual{};
    if (::fstatat(parent, name.c_str(), &actual, AT_SYMLINK_NOFOLLOW) < 0) {
      return errno == ENOENT && !expected.content;
    }
    return expected.content && sameRevision(actual, expected.revision);
  }

  struct TemporaryFile {
    int parent;
    std::string name;
    Descriptor file;
    ~TemporaryFile() {
      if (!name.empty()) {
        ::unlinkat(parent, name.c_str(), 0);
      }
    }
  };

  std::expected<std::string, std::string> uniqueName(const std::string& target) {
    std::uint64_t nonce = 0;
    auto* bytes = reinterpret_cast<char*>(&nonce);
    std::size_t offset = 0;
    while (offset < sizeof(nonce)) {
      const auto count = ::getrandom(bytes + offset, sizeof(nonce) - offset, GRND_NONBLOCK);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return std::unexpected(failure("create random temporary name", count == 0 ? EIO : errno));
      }
      offset += static_cast<std::size_t>(count);
    }
    return std::format(".{}.tmp.{:016x}", target, nonce);
  }

  // Adapted from Noctalia's writeAll helper; handle short writes and EINTR.
  bool writeAll(int fd, std::string_view content) {
    while (!content.empty()) {
      const auto count = ::write(fd, content.data(), content.size());
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        if (count == 0) {
          errno = EIO;
        }
        return false;
      }
      content.remove_prefix(static_cast<std::size_t>(count));
    }
    return true;
  }
} // namespace

files::AtomicWriteResult
files::updateTextFileAtomic(const std::filesystem::path& path, const AtomicTransform& transform) {
  if (!path.is_absolute()
      || path.native().contains('\0')
      || path.filename().empty()
      || path.filename() == "."
      || path.filename() == "..") {
    return {.error = "expected an absolute application-owned file path"};
  }
  auto parent = openParent(path.parent_path());
  if (!parent) {
    return {.error = std::move(parent.error())};
  }
  const auto name = path.filename().native();
  const auto lockName = std::format(".{}.lock", name);
  const Descriptor lock(
      ::openat(parent->fd, lockName.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600)
  );
  if (lock.fd < 0) {
    return {.error = failure("open settings lock")};
  }
  struct stat lockInfo{};
  if (::fstat(lock.fd, &lockInfo) < 0
      || !S_ISREG(lockInfo.st_mode)
      || lockInfo.st_nlink != 1
      || lockInfo.st_uid != ::geteuid()) {
    return {.error = "settings lock must be an owned regular file with no links"};
  }
  if (::flock(lock.fd, LOCK_EX | LOCK_NB) < 0) {
    return {.error = failure("settings are busy; retry later")};
  }
  if (::fchmod(lock.fd, 0600) < 0) {
    return {.error = failure("secure settings lock")};
  }
  const auto current = readCurrent(parent->fd, name);
  if (!current) {
    return {.error = current.error()};
  }
  auto transformed = transform(current->content);
  if (!transformed) {
    return {.error = std::move(transformed.error())};
  }
  if (!*transformed || *transformed == current->content) {
    return {.state = AtomicWriteState::UNCHANGED, .error = {}};
  }
  const auto& content = **transformed;
  if (content.size() > kMaxBytes) {
    return {.error = "settings exceed the 256 KiB limit"};
  }
  TemporaryFile temporary{.parent = parent->fd, .name = {}, .file = Descriptor{}};
  for (int attempt = 0; attempt < 32; ++attempt) {
    auto randomName = uniqueName(name);
    if (!randomName) {
      return {.error = std::move(randomName.error())};
    }
    const int fd =
        ::openat(parent->fd, randomName->c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
      temporary.name = std::move(*randomName);
      temporary.file = Descriptor(fd);
      break;
    }
    if (errno != EEXIST) {
      return {.error = failure("create settings temporary file")};
    }
  }
  if (temporary.file.fd < 0) {
    return {.error = "could not allocate a unique settings temporary file"};
  }
  if (::fchmod(temporary.file.fd, 0600) < 0 || !writeAll(temporary.file.fd, content) || !sync(temporary.file.fd)) {
    return {.error = failure("write and sync settings")};
  }
  if (temporary.file.close() < 0) {
    return {.error = failure("close settings temporary file")};
  }
  if (!stillCurrent(parent->fd, name, *current)) {
    return {.error = "settings changed before replacement; reload and retry"};
  }
  const long renamed = current->content
      ? ::renameat(parent->fd, temporary.name.c_str(), parent->fd, name.c_str())
      : ::syscall(SYS_renameat2, parent->fd, temporary.name.c_str(), parent->fd, name.c_str(), RENAME_NOREPLACE);
  if (renamed < 0) {
    return {.error = failure("replace settings")};
  }
  temporary.name.clear();
  if (!sync(parent->fd)) {
    return {.state = AtomicWriteState::COMMITTED, .error = failure("settings replaced but directory sync failed")};
  }
  return {.state = AtomicWriteState::DURABLE, .error = {}};
}
