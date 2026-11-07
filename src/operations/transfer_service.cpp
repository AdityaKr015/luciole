#include "operations/transfer_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <limits>
#include <linux/fs.h>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace {
  class Fd {
  public:
    explicit Fd(int fd = -1) : m_fd(fd) {}
    ~Fd() {
      if (m_fd >= 0) {
        ::close(m_fd);
      }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
      if (this != &other) {
        if (m_fd >= 0) {
          ::close(m_fd);
        }
        m_fd = std::exchange(other.m_fd, -1);
      }
      return *this;
    }
    int get() const { return m_fd; }

  private:
    int m_fd;
  };
  struct TransferCancelled : std::runtime_error {
    TransferCancelled() : std::runtime_error("Transfer cancelled.") {}
  };
  [[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
  }
  Fd opened(int fd) {
    if (fd < 0) {
      fail("Could not open item");
    }
    return Fd(fd);
  }
  struct stat info(int parent, const std::string& name) {
    struct stat value{};
    if (::fstatat(parent, name.c_str(), &value, AT_SYMLINK_NOFOLLOW) != 0) {
      fail("Could not inspect item");
    }
    return value;
  }
  struct stat info(int fd) {
    struct stat value{};
    if (::fstat(fd, &value) != 0) {
      fail("Could not inspect folder");
    }
    return value;
  }
  bool sameIdentity(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && (a.st_mode & S_IFMT) == (b.st_mode & S_IFMT);
  }
  bool unchanged(const struct stat& a, const struct stat& b) {
    return sameIdentity(a, b)
        && a.st_mode == b.st_mode
        && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec
        && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec
        && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
  }
  bool sameContent(const struct stat& a, const struct stat& b) {
    return sameIdentity(a, b)
        && a.st_mode == b.st_mode
        && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec
        && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec;
  }
  void requireUnchanged(const struct stat& expected, const struct stat& actual) {
    if (!unchanged(expected, actual)) {
      throw std::runtime_error("The source changed during the transfer.");
    }
  }
  Fd directory(int parent, const std::string& name) {
    return opened(::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  }
  std::vector<std::string> children(int fd, std::size_t limit = std::numeric_limits<std::size_t>::max()) {
    const int duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
      fail("Could not read folder");
    }
    struct CloseDirectory {
      void operator()(DIR* stream) const { ::closedir(stream); }
    };
    std::unique_ptr<DIR, CloseDirectory> stream(::fdopendir(duplicate));
    if (!stream) {
      ::close(duplicate);
      fail("Could not read folder");
    }
    ::rewinddir(stream.get());
    std::vector<std::string> result;
    for (;;) {
      errno = 0;
      const auto* entry = ::readdir(stream.get());
      if (entry == nullptr) {
        if (errno != 0) {
          fail("Could not read folder");
        }
        break;
      }
      const std::string name(entry->d_name);
      if (name != "." && name != "..") {
        if (result.size() >= limit) {
          throw std::runtime_error("The merge exceeds its 16384-item limit. Select smaller folders.");
        }
        result.push_back(name);
      }
    }
    return result;
  }
  bool hasAncestor(int fd, const struct stat& ancestor) {
    auto current = opened(::fcntl(fd, F_DUPFD_CLOEXEC, 0));
    for (std::size_t depth = 0; depth < 65536; ++depth) {
      const auto here = info(current.get());
      if (sameIdentity(here, ancestor)) {
        return true;
      }
      auto parent = directory(current.get(), "..");
      if (sameIdentity(here, info(parent.get()))) {
        return false;
      }
      // Keep the old directory alive until the new descriptor is acquired.
      const int next = ::fcntl(parent.get(), F_DUPFD_CLOEXEC, 0);
      if (next < 0) {
        fail("Could not inspect parent folder");
      }
      current = Fd(next);
    }
    throw std::runtime_error("The folder ancestry is too deep.");
  }
  struct Source;
  struct Work {
    std::atomic<bool>& cancelled;
    std::atomic<std::uint64_t>& bytes;
    using Choose = std::function<files::TransferConflictChoice(
        const Source&, const files::Location&, const std::string&, const struct stat&, bool
    )>;
    Choose choose;
    bool allowMerge = true;
    std::size_t mergedItems = 0;
    std::map<std::pair<dev_t, ino_t>, struct stat> changedLinks;
    std::vector<char> buffer;
    bool metadataIncomplete = false;
    Work(std::atomic<bool>& cancelled, std::atomic<std::uint64_t>& bytes, Choose choose)
        : cancelled(cancelled), bytes(bytes), choose(std::move(choose)), buffer(128 * 1024) {}
    struct stat expected(const struct stat& original) const {
      const auto found = changedLinks.find({original.st_dev, original.st_ino});
      return found != changedLinks.end() && sameContent(original, found->second) ? found->second : original;
    }
    void note(const struct stat& value) { changedLinks[{value.st_dev, value.st_ino}] = value; }
    void check() const {
      if (cancelled.load()) {
        throw TransferCancelled();
      }
    }
  };
  struct Tree {
    std::string name;
    struct stat metadata;
    std::vector<Tree> children;
  };
  void copyAttributes(int from, int to, Work& work) {
    const auto count = ::flistxattr(from, nullptr, 0);
    if (count < 0) {
      if (errno == ENOTSUP || errno == EOPNOTSUPP) {
        return;
      }
      fail("Could not read file attributes");
    }
    if (count == 0) {
      return;
    }
    if (count > 1024 * 1024) {
      throw std::runtime_error("File attributes are too large.");
    }
    std::vector<char> names(static_cast<std::size_t>(count));
    if (::flistxattr(from, names.data(), names.size()) != count) {
      fail("File attributes changed");
    }
    for (std::size_t offset = 0; offset < names.size();) {
      work.check();
      const auto length = ::strnlen(names.data() + offset, names.size() - offset);
      if (length == names.size() - offset) {
        throw std::runtime_error("Invalid file attributes.");
      }
      const std::string key(names.data() + offset, length);
      offset += length + 1;
      if (!key.starts_with("user.") && key != "system.posix_acl_access" && key != "system.posix_acl_default") {
        continue;
      }
      const auto size = ::fgetxattr(from, key.c_str(), nullptr, 0);
      if (size < 0) {
        fail("Could not read file attribute");
      }
      if (size > 1024 * 1024) {
        throw std::runtime_error("A file attribute is too large.");
      }
      std::vector<char> value(static_cast<std::size_t>(size));
      if (::fgetxattr(from, key.c_str(), value.data(), value.size()) != size) {
        fail("File attributes changed");
      }
      if (::fsetxattr(to, key.c_str(), value.data(), value.size(), 0) != 0) {
        if (errno == ENOTSUP || errno == EOPNOTSUPP) {
          work.metadataIncomplete = true;
        } else {
          fail("Could not preserve file attributes");
        }
      }
    }
  }
  void metadata(int input, int fd, const struct stat& source, Work& work) {
    const timespec times[]{source.st_atim, source.st_mtim};
    if (::fchmod(fd, source.st_mode & 0777) != 0 || ::futimens(fd, times) != 0) {
      fail("Could not preserve file metadata");
    }
    copyAttributes(input, fd, work);
    if (::fsync(fd) != 0) {
      fail("Could not synchronize copied data");
    }
  }
  Tree copyTree(int from, const std::string& name, int to, const std::string& target, Work& work, unsigned depth) {
    work.check();
    if (depth > 256) {
      throw std::runtime_error("The folder tree is too deep.");
    }
    Tree tree{.name = name, .metadata = info(from, name), .children = {}};
    const auto& source = tree.metadata;
    if (S_ISREG(source.st_mode)) {
      auto input = opened(::openat(from, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
      requireUnchanged(source, info(input.get()));
      auto output = opened(::openat(to, target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
      auto& buffer = work.buffer;
      for (;;) {
        work.check();
        const auto size = ::read(input.get(), buffer.data(), buffer.size());
        if (size < 0) {
          if (errno == EINTR) {
            continue;
          }
          fail("Could not read source");
        }
        if (size == 0) {
          break;
        }
        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(size)) {
          work.check();
          const auto written = ::write(output.get(), buffer.data() + offset, static_cast<std::size_t>(size) - offset);
          if (written < 0) {
            if (errno == EINTR) {
              continue;
            }
            fail("Could not write destination");
          }
          if (written == 0) {
            throw std::runtime_error("The destination stopped accepting data.");
          }
          offset += static_cast<std::size_t>(written);
          work.bytes.fetch_add(static_cast<std::uint64_t>(written));
        }
      }
      requireUnchanged(source, info(input.get()));
      metadata(input.get(), output.get(), source, work);
    } else if (S_ISDIR(source.st_mode)) {
      auto input = directory(from, name);
      requireUnchanged(source, info(input.get()));
      if (::mkdirat(to, target.c_str(), 0700) != 0) {
        fail("Could not create destination folder");
      }
      auto output = directory(to, target);
      for (const auto& child : children(input.get())) {
        tree.children.push_back(copyTree(input.get(), child, output.get(), child, work, depth + 1));
      }
      requireUnchanged(source, info(input.get()));
      metadata(input.get(), output.get(), source, work);
    } else if (S_ISLNK(source.st_mode)) {
      std::vector<char> buffer(256);
      for (;;) {
        const auto size = ::readlinkat(from, name.c_str(), buffer.data(), buffer.size());
        if (size < 0) {
          fail("Could not read symbolic link");
        }
        if (static_cast<std::size_t>(size) < buffer.size()) {
          const std::string link(buffer.data(), static_cast<std::size_t>(size));
          if (::symlinkat(link.c_str(), to, target.c_str()) != 0) {
            fail("Could not copy symbolic link");
          }
          break;
        }
        if (buffer.size() >= 1024 * 1024) {
          throw std::runtime_error("The symbolic link is too long.");
        }
        buffer.resize(buffer.size() * 2);
      }
      requireUnchanged(source, info(from, name));
      const timespec times[]{source.st_atim, source.st_mtim};
      if (::utimensat(to, target.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0) {
        fail("Could not preserve link metadata");
      }
    } else {
      throw std::runtime_error("Special files cannot be transferred.");
    }
    return tree;
  }
  void verifyTree(int parent, const Tree& tree, Work& work) {
    work.check();
    requireUnchanged(work.expected(tree.metadata), info(parent, tree.name));
    if (S_ISDIR(tree.metadata.st_mode)) {
      auto fd = directory(parent, tree.name);
      requireUnchanged(tree.metadata, info(fd.get()));
      if (children(fd.get()).size() != tree.children.size()) {
        throw std::runtime_error("The source folder changed.");
      }
      for (const auto& child : tree.children) {
        verifyTree(fd.get(), child, work);
      }
    }
  }
  void removeSource(int parent, const Tree& tree, Work& work) {
    work.check();
    auto pinned = opened(::openat(parent, tree.name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
    requireUnchanged(work.expected(tree.metadata), info(pinned.get()));
    requireUnchanged(work.expected(tree.metadata), info(parent, tree.name));
    const bool folder = S_ISDIR(tree.metadata.st_mode);
    if (folder) {
      auto fd = directory(parent, tree.name);
      requireUnchanged(tree.metadata, info(fd.get()));
      for (const auto& child : tree.children) {
        removeSource(fd.get(), child, work);
      }
      if (!sameIdentity(tree.metadata, info(parent, tree.name))) {
        throw std::runtime_error("The source folder changed.");
      }
    }
    if (::unlinkat(parent, tree.name.c_str(), folder ? AT_REMOVEDIR : 0) != 0) {
      fail("Could not remove copied source");
    }
    work.note(info(pinned.get()));
  }
  void removeTemporary(int parent, const std::string& name, Work& work) {
    const auto value = info(parent, name);
    auto pinned = opened(::openat(parent, name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
    const bool folder = S_ISDIR(value.st_mode);
    if (folder) {
      auto fd = directory(parent, name);
      if (::fchmod(fd.get(), 0700) != 0) {
        fail("Could not clean temporary folder");
      }
      for (const auto& child : children(fd.get())) {
        removeTemporary(fd.get(), child, work);
      }
    }
    if (::unlinkat(parent, name.c_str(), folder ? AT_REMOVEDIR : 0) != 0) {
      fail("Could not clean temporary item");
    }
    work.note(info(pinned.get()));
  }
  class Stage {
  public:
    Stage(int parent, Work& work) : m_parent(parent), m_work(work) {
      const files::GString id(g_uuid_string_random());
      m_name = ".noctalia-files-transfer-" + std::string(id.get());
      if (::mkdirat(parent, m_name.c_str(), 0700) != 0) {
        fail("Could not prepare destination");
      }
      try {
        m_fd = directory(parent, m_name);
      } catch (...) {
        ::unlinkat(parent, m_name.c_str(), AT_REMOVEDIR);
        throw;
      }
    }
    ~Stage() {
      if (!m_cleaned) {
        (void)clear();
      }
    }
    bool clear() noexcept {
      if (m_cleaned) {
        return true;
      }
      m_cleaned = true;
      try {
        for (const auto& name : children(m_fd.get())) {
          removeTemporary(m_fd.get(), name, m_work);
        }
        return ::unlinkat(m_parent, m_name.c_str(), AT_REMOVEDIR) == 0;
      } catch (...) {
        return false;
      }
    }
    int fd() const { return m_fd.get(); }
    const std::string& name() const { return m_name; }
    void retain() { m_cleaned = true; }

  private:
    int m_parent;
    Work& m_work;
    std::string m_name;
    Fd m_fd;
    bool m_cleaned = false;
  };
  struct Source {
    std::size_t index;
    files::Location location;
    Fd parent;
    std::string name;
    struct stat metadata;
    std::string parentPath;
    struct stat parentMetadata;
  };
  Fd openSourceParent(const Source& source) {
    auto fd = opened(::open(source.parentPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!sameIdentity(source.parentMetadata, info(fd.get()))) {
      throw std::runtime_error("The source folder changed.");
    }
    return fd;
  }
  std::string numberedName(const Source& source, int destination, std::uint64_t number) {
    const auto dot = source.name.find_last_of('.');
    const bool extension = !S_ISDIR(source.metadata.st_mode) && dot != std::string::npos && dot != 0;
    const auto suffix = extension ? source.name.substr(dot) : std::string{};
    auto stem = extension ? source.name.substr(0, dot) : source.name;
    const auto counter = " (" + std::to_string(number) + ")";
    const auto limit = ::fpathconf(destination, _PC_NAME_MAX);
    if (limit > 0 && stem.size() + counter.size() + suffix.size() > static_cast<std::size_t>(limit)) {
      if (counter.size() + suffix.size() >= static_cast<std::size_t>(limit)) {
        throw std::runtime_error("The numbered filename would be too long.");
      }
      auto bytes = static_cast<std::size_t>(limit) - counter.size() - suffix.size();
      if (g_utf8_validate(stem.data(), static_cast<gssize>(stem.size()), nullptr)) {
        while (bytes > 0 && (static_cast<unsigned char>(stem[bytes]) & 0xc0U) == 0x80U) {
          --bytes;
        }
      }
      stem.resize(bytes);
    }
    return stem + counter + suffix;
  }
  files::Location childLocation(const files::Location& parent, const std::string& name) {
    const files::GioPtr<GFile> child(g_file_get_child(parent.file().get(), name.c_str()));
    const files::GString uri(g_file_get_uri(child.get()));
    return files::Location::fromUri(uri.get());
  }
  struct Destination {
    files::Location location;
    struct stat metadata;
    std::shared_ptr<const Destination> ancestor;
  };
  void validateDestination(const Destination& destination) {
    for (const auto* current = &destination; current; current = current->ancestor.get()) {
      struct stat value{};
      const auto path = *current->location.nativePath();
      const int result = current->ancestor ? ::lstat(path.c_str(), &value) : ::stat(path.c_str(), &value);
      if (result != 0 || !sameIdentity(value, current->metadata)) {
        throw std::runtime_error("The destination folder moved or was replaced. Start a new transfer.");
      }
    }
  }
  struct Node {
    Node(Source captured, std::shared_ptr<const Destination> target, files::TransferResult outcome, unsigned level = 0)
        : source(std::move(captured)), destination(std::move(target)), result(std::move(outcome)), depth(level) {}
    Source source;
    std::shared_ptr<const Destination> destination;
    files::TransferResult result;
    std::vector<std::shared_ptr<Node>> children;
    bool merging = false;
    unsigned depth = 0;
    bool attempted = false;
    std::shared_ptr<const Destination> mergeTarget;
  };
  struct Job {
    explicit Job(files::TransferRequest transfer) : request(std::move(transfer)) {}
    files::TransferRequest request;
    std::vector<std::shared_ptr<Node>> nodes;
    std::map<std::pair<dev_t, ino_t>, struct stat> changedLinks;
    std::size_t mergedItems = 0;
    std::vector<files::TransferResult> outcomes;
    std::string report;
  };
  void execute(Node& node, files::TransferAction action, Work& work, bool retry);
  files::TransferResult
  merge(Node& node, int destination, files::TransferAction action, Work& work, const struct stat& existing);
  void validateBindings(const Source& source, const Destination& destination);
  bool chooseTarget(
      const Source& source, int destination, Work& work, std::string& target, bool& keepBoth, std::uint64_t& number,
      std::optional<struct stat>& replace, bool& merging, const Destination& binding, bool forcePrompt = false
  ) {
    for (;;) {
      work.check();
      if (keepBoth) {
        target = numberedName(source, destination, ++number);
      }
      struct stat existing{};
      if (::fstatat(destination, target.c_str(), &existing, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
          replace.reset();
          return true;
        }
        fail("Could not inspect destination");
      }
      if (keepBoth) {
        continue;
      }
      if (replace && unchanged(*replace, existing)) {
        return true;
      }
      replace.reset();
      validateBindings(source, binding);
      const auto choice = work.choose(source, binding.location, target, existing, forcePrompt);
      validateBindings(source, binding);
      requireUnchanged(work.expected(source.metadata), info(source.parent.get(), source.name));
      switch (choice) {
      case files::TransferConflictChoice::Merge:
        if (!work.allowMerge || !S_ISDIR(source.metadata.st_mode) || !S_ISDIR(existing.st_mode)) {
          throw std::runtime_error("Only two actual folders can be merged.");
        }
        replace = existing;
        merging = true;
        return true;
      case files::TransferConflictChoice::Skip:
        return false;
      case files::TransferConflictChoice::Cancel:
        work.cancelled.store(true);
        throw TransferCancelled();
      case files::TransferConflictChoice::KeepBoth:
        keepBoth = true;
        break;
      case files::TransferConflictChoice::Replace:
        replace = existing;
        return true;
      }
    }
  }
  files::TransferResult transfer(Node& node, int destination, files::TransferAction action, Work& work) {
    auto& source = node.source;
    const auto& binding = *node.destination;
    using enum files::TransferOutcome;
    files::TransferResult result{source.location};
    try {
      work.check();
      source.metadata = work.expected(source.metadata);
      requireUnchanged(source.metadata, info(source.parent.get(), source.name));
      const bool sameFolder = sameIdentity(info(source.parent.get()), info(destination));
      if (sameFolder && action == files::TransferAction::Move) {
        result.outcome = Skipped;
        return result;
      }
      if (S_ISDIR(source.metadata.st_mode) && hasAncestor(destination, source.metadata)) {
        throw std::runtime_error("A folder cannot be placed inside itself.");
      }
      if (!S_ISDIR(source.metadata.st_mode) && !S_ISREG(source.metadata.st_mode) && !S_ISLNK(source.metadata.st_mode)) {
        throw std::runtime_error("Special files cannot be transferred.");
      }
      std::string target = source.name;
      bool keepBoth = sameFolder, merging = false, forcePrompt = false;
      std::uint64_t number = 0;
      std::optional<struct stat> replace;
      for (;;) {
        if (!chooseTarget(
                source, destination, work, target, keepBoth, number, replace, merging, binding, forcePrompt
            )) {
          result.outcome = Skipped;
          return result;
        }
        if (merging) {
          if (!unchanged(*replace, info(destination, target))) {
            merging = false;
            replace.reset();
            forcePrompt = true;
            continue;
          }
          return merge(node, destination, action, work, *replace);
        }
        validateBindings(source, binding);
        if (action == files::TransferAction::Copy || replace) {
          break;
        }
        work.check();
        requireUnchanged(source.metadata, info(source.parent.get(), source.name));
        if (::syscall(
                SYS_renameat2, source.parent.get(), source.name.c_str(), destination, target.c_str(), RENAME_NOREPLACE
            )
            == 0) {
          work.note(info(destination, target));
          result.outcome = Moved;
          result.destination = childLocation(binding.location, target);
          return result;
        }
        if (errno == EXDEV) {
          break;
        }
        if (errno != EEXIST) {
          fail("Could not move item");
        }
      }
      Stage stage(destination, work);
      work.metadataIncomplete = false;
      bool published = false;
      try {
        const auto tree = copyTree(source.parent.get(), source.name, stage.fd(), "item", work, 0);
        requireUnchanged(source.metadata, tree.metadata);
        verifyTree(source.parent.get(), tree, work);
        for (;;) {
          work.check();
          if (replace) {
            struct stat current{};
            if (::fstatat(destination, target.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0
                || !unchanged(*replace, current)) {
              replace.reset();
              if (!chooseTarget(source, destination, work, target, keepBoth, number, replace, merging, binding, true)) {
                result.outcome = Skipped;
                break;
              }
              continue;
            }
          }
          if (merging) {
            // The conflicting folder appeared after staging. No staged item has been published.
            if (!stage.clear()) {
              throw std::runtime_error("Could not remove staging before merging: " + stage.name());
            }
            return merge(node, destination, action, work, *replace);
          }
          validateBindings(source, binding);
          verifyTree(source.parent.get(), tree, work);
          const auto flags = replace ? RENAME_EXCHANGE : RENAME_NOREPLACE;
          if (::syscall(SYS_renameat2, stage.fd(), "item", destination, target.c_str(), flags) != 0) {
            if (errno == EEXIST || (replace && errno == ENOENT)) {
              replace.reset();
              if (!chooseTarget(source, destination, work, target, keepBoth, number, replace, merging, binding, true)) {
                result.outcome = Skipped;
                break;
              }
              continue;
            }
            fail("Could not place item in destination");
          }
          published = true;
          result.destination = childLocation(binding.location, target);
          if (replace && !sameContent(*replace, info(stage.fd(), "item"))) {
            stage.retain();
            throw std::runtime_error("The previous destination changed and was retained in " + stage.name());
          }
          if (replace) {
            work.note(info(stage.fd(), "item"));
          }
          result.outcome = Copied;
          if (action == files::TransferAction::Move) {
            verifyTree(source.parent.get(), tree, work);
            removeSource(source.parent.get(), tree, work);
            result.outcome = Moved;
          }
          break;
        }
      } catch (const TransferCancelled&) {
        result.outcome = published ? Partial : Cancelled;
        result.error =
            published ? "The copy completed; cancellation left source items in place." : "Transfer cancelled.";
      } catch (const std::exception& error) {
        result.outcome = published ? Partial : Failed;
        result.error = published
            ? "The destination was created, but transfer cleanup is incomplete: " + std::string(error.what())
            : error.what();
      }
      if (!stage.clear()) {
        result.outcome = Partial;
        result.error += " Previous or temporary items remain in " + stage.name() + " in the destination.";
      }
      if (work.metadataIncomplete && published) {
        result.error += " The destination does not support some extended attributes.";
      }
    } catch (const TransferCancelled&) {
      result.outcome = Cancelled;
      result.error = "Transfer cancelled.";
    } catch (const std::exception& error) {
      result.error = error.what();
    }
    result.retryable = result.outcome == Failed;
    return result;
  }

  std::size_t retryCount(const Node& node) {
    std::size_t count = node.result.retryable ? 1 : 0;
    for (const auto& child : node.children) {
      count += retryCount(*child);
    }
    return count;
  }
  files::TransferResult snapshot(const Node& node) {
    auto result = node.result;
    result.children.clear();
    for (const auto& child : node.children) {
      result.children.push_back(snapshot(*child));
    }
    return result;
  }
  void validateBindings(const Source& source, const Destination& destination) {
    const auto parent = openSourceParent(source);
    if (!sameIdentity(info(parent.get()), info(source.parent.get()))) {
      throw std::runtime_error("The source parent moved or was replaced.");
    }
    validateDestination(destination);
  }
  files::TransferResult
  merge(Node& node, int destination, files::TransferAction action, Work& work, const struct stat& existing) {
    using enum files::TransferOutcome;
    auto& source = node.source;
    files::TransferResult result{source.location};
    result.destination = childLocation(node.destination->location, source.name);
    bool finishedChildren = false;
    try {
      work.check();
      validateBindings(source, *node.destination);
      auto input = directory(source.parent.get(), source.name);
      if (!sameIdentity(info(input.get()), source.metadata)
          || !sameIdentity(info(source.parent.get(), source.name), source.metadata)) {
        throw std::runtime_error("The source folder changed.");
      }
      auto output = directory(destination, source.name);
      if (!sameIdentity(existing, info(output.get()))) {
        throw std::runtime_error("The destination folder changed.");
      }
      if (hasAncestor(output.get(), source.metadata) || hasAncestor(input.get(), existing)) {
        throw std::runtime_error("Related or identical folders cannot be merged.");
      }
      if (!node.merging) {
        requireUnchanged(source.metadata, info(input.get()));
        requireUnchanged(existing, info(destination, source.name));
        auto names = children(input.get(), 16384 - work.mergedItems);
        if (node.depth >= 256 || names.size() > 16384 - work.mergedItems) {
          throw std::runtime_error("The merge exceeds 16384 items or 256 folder levels. Select smaller folders.");
        }
        work.mergedItems += names.size();
        std::ranges::sort(names);
        const auto childPath = *node.destination->location.nativePath() + "/" + source.name;
        auto target = std::make_shared<const Destination>(
            Destination{files::Location::fromPath(childPath), existing, node.destination}
        );
        std::vector<std::shared_ptr<Node>> planned;
        for (const auto& name : names) {
          work.check();
          const auto metadata = info(input.get(), name);
          auto location = files::Location::fromPath(*source.location.nativePath() + "/" + name);
          Source child{source.index, location, Fd{}, name, metadata, *source.location.nativePath(), info(input.get())};
          planned.push_back(
              std::make_shared<Node>(
                  std::move(child), target, files::TransferResult{location, Cancelled, "Not attempted."}, node.depth + 1
              )
          );
        }
        requireUnchanged(source.metadata, info(input.get()));
        node.children = std::move(planned);
        node.mergeTarget = target;
        node.merging = true;
      }
      // Each child has its own commit boundary. Existing destination directory
      // metadata and destination-only entries are preserved.
      for (const auto& child : node.children) {
        validateBindings(source, *node.destination);
        validateDestination(*node.mergeTarget);
        if (!sameIdentity(source.metadata, info(source.parent.get(), source.name))) {
          throw std::runtime_error("The source folder moved or was replaced.");
        }
        execute(*child, action, work, false);
      }
      finishedChildren = true;
      const bool complete = std::ranges::all_of(node.children, [action](const auto& child) {
        return child->result.outcome == (action == files::TransferAction::Copy ? Copied : Moved);
      });
      if (complete) {
        validateBindings(source, *node.destination);
        validateDestination(*node.mergeTarget);
        if (!sameIdentity(source.metadata, info(source.parent.get(), source.name))) {
          throw std::runtime_error("The source folder moved or was replaced.");
        }
        if (action == files::TransferAction::Move) {
          work.check();
          if (!children(input.get()).empty()) {
            throw std::runtime_error("New source entries remain; they were not included in this merge.");
          }
          if (::unlinkat(source.parent.get(), source.name.c_str(), AT_REMOVEDIR) != 0) {
            fail("Could not remove empty merged source folder");
          }
          result.outcome = Moved;
        } else {
          requireUnchanged(source.metadata, info(input.get()));
          result.outcome = Copied;
        }
        return result;
      }
    } catch (const TransferCancelled&) {
      result.error = "Merge cancelled. Completed child transfers remain applied.";
    } catch (const std::exception& error) {
      result.error = error.what();
      // Only an empty source-folder cleanup can be retried here. Publication or
      // source-removal partials of individual files are never replayed.
      result.retryable = finishedChildren
          && action == files::TransferAction::Move
          && std::ranges::all_of(node.children, [](const auto& child) { return child->result.outcome == Moved; });
    }
    std::size_t done = 0, failed = 0, skipped = 0, cancelled = 0;
    for (const auto& child : node.children) {
      const auto value = child->result.outcome;
      done += value == Moved || value == Copied || value == Partial ? 1 : 0;
      failed += value == Failed || value == Partial ? 1 : 0;
      skipped += value == Skipped ? 1 : 0;
      cancelled += value == Cancelled ? 1 : 0;
    }
    result.outcome = done > 0                 ? Partial
        : failed > 0 || !result.error.empty() ? (work.cancelled.load() ? Cancelled : Failed)
        : cancelled > 0                       ? Cancelled
                                              : Skipped;
    result.retryable = result.retryable || (!node.merging && result.outcome == Failed);
    if (result.error.empty()) {
      result.error = std::format(
          "Merge: {} completed, {} failed/partial, {} skipped, {} cancelled.", done, failed, skipped, cancelled
      );
      const auto problem =
          std::ranges::find_if(node.children, [](const auto& child) { return !child->result.error.empty(); });
      if (problem != node.children.end()) {
        result.error += " " + (*problem)->source.location.uri() + ": " + (*problem)->result.error;
      }
    }
    return result;
  }
  void execute(Node& node, files::TransferAction action, Work& work, bool retry) {
    // Completed/skipped/partial leaf operations are terminal. A retry visits only
    // captured failures, or an existing merge container with failed descendants.
    if (retry && retryCount(node) == 0) {
      return;
    }
    if (!retry && node.attempted) {
      return;
    }
    if (node.merging && retry) {
      for (auto& child : node.children) {
        execute(*child, action, work, true);
      }
    }
    try {
      work.check();
      node.source.parent = openSourceParent(node.source);
      validateBindings(node.source, *node.destination);
      auto destination =
          opened(::open(node.destination->location.nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      if (!sameIdentity(info(destination.get()), node.destination->metadata)) {
        throw std::runtime_error("The destination folder changed. Start a new transfer.");
      }
      if (node.merging) {
        const auto existing = info(destination.get(), node.source.name);
        // The child destination retains the original merge target's identity.
        validateDestination(*node.mergeTarget);
        node.result = merge(node, destination.get(), action, work, existing);
      } else {
        requireUnchanged(work.expected(node.source.metadata), info(node.source.parent.get(), node.source.name));
        node.result = transfer(node, destination.get(), action, work);
      }
    } catch (const TransferCancelled&) {
      const bool effects = std::ranges::any_of(node.children, [](const auto& child) {
        return child->result.outcome == files::TransferOutcome::Moved
            || child->result.outcome == files::TransferOutcome::Copied
            || child->result.outcome == files::TransferOutcome::Partial;
      });
      node.result = {
          node.source.location, effects ? files::TransferOutcome::Partial : files::TransferOutcome::Cancelled,
          "Transfer cancelled. Completed child transfers remain applied."
      };
    } catch (const std::exception& error) {
      node.result = {
          node.source.location, node.merging ? files::TransferOutcome::Partial : files::TransferOutcome::Failed,
          error.what()
      };
    }
    node.source.parent = Fd{};
    node.attempted = true;
  }
  std::string jobReport(const std::vector<files::TransferResult>& results) {
    std::string report = "Transfer results\n";
    constexpr std::array labels{"Moved", "Copied", "Skipped", "Failed", "Cancelled", "Partial"};
    std::function<void(const files::TransferResult&)> append = [&](const auto& result) {
      if (report.size() > 16 * 1024 * 1024) {
        return;
      }
      report += result.source.uri()
          + (result.destination ? " -> " + result.destination->uri() : "")
          + "\n  "
          + labels.at(static_cast<std::size_t>(result.outcome));
      report += result.retryable ? " (retry available)" : "";
      report += result.error.empty() ? "\n" : "\n  " + result.error + "\n";
      for (const auto& child : result.children) {
        append(child);
      }
    };
    for (const auto& result : results) {
      append(result);
    }
    if (report.size() > 16 * 1024 * 1024) {
      report += "\nReport truncated.\n";
    }
    return report;
  }

} // namespace

struct files::TransferService::State {
  std::function<void()> changed;
  bool active = false;
  TransferAction action = TransferAction::Move;
  std::size_t total = 0;
  std::atomic<std::size_t> completed{0};
  std::atomic<std::uint64_t> bytes{0};
  std::atomic<bool> cancelled{false};
  std::vector<TransferResult> results;
  mutable std::mutex mutex;
  std::condition_variable decision;
  std::optional<TransferConflict> conflict;
  std::optional<TransferConflictChoice> choice;
  std::uint64_t nextConflict = 0, jobId = 0;
  bool applyToAll = false;
  std::shared_ptr<Job> job;
  std::size_t retryable = 0;
  std::string report;

  static void notify(const std::shared_ptr<State>& owner) {
    auto* source = g_idle_source_new();
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(
        source,
        [](gpointer data) -> gboolean {
          const auto& retained = *static_cast<std::shared_ptr<State>*>(data);
          const auto callback = retained->changed;
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
};

files::TransferService::TransferService() : m_state(std::make_shared<State>()) {}
files::TransferService::~TransferService() {
  m_state->changed = {};
  cancel();
}
void files::TransferService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
bool files::TransferService::busy() const { return m_state->active; }
std::size_t files::TransferService::total() const { return m_state->total; }
std::size_t files::TransferService::completed() const { return m_state->completed.load(); }
std::uint64_t files::TransferService::bytes() const { return m_state->bytes.load(); }
files::TransferAction files::TransferService::action() const { return m_state->action; }
const std::vector<files::TransferResult>& files::TransferService::results() const { return m_state->results; }
void files::TransferService::cancel() {
  m_state->cancelled.store(true);
  m_state->decision.notify_all();
}
std::optional<files::TransferConflict> files::TransferService::conflict() const {
  const std::scoped_lock lock(m_state->mutex);
  return m_state->conflict;
}
bool files::TransferService::resolve(std::uint64_t id, TransferConflictChoice choice, bool applyToAll) {
  {
    const std::scoped_lock lock(m_state->mutex);
    if (!m_state->conflict
        || m_state->conflict->id != id
        || m_state->choice
        || choice < TransferConflictChoice::Replace
        || choice > TransferConflictChoice::Merge
        || (choice == TransferConflictChoice::Merge && !m_state->conflict->canMerge)
        || (applyToAll && (!m_state->conflict->canApplyToAll || choice == TransferConflictChoice::Cancel))) {
      return false;
    }
    m_state->choice = choice;
    m_state->applyToAll = applyToAll;
    if (choice == TransferConflictChoice::Cancel) {
      m_state->cancelled.store(true);
    }
  }
  m_state->decision.notify_all();
  return true;
}

std::size_t files::TransferService::retryable() const { return busy() ? 0 : m_state->retryable; }
const std::string& files::TransferService::report() const { return m_state->report; }
std::uint64_t files::TransferService::jobId() const { return m_state->jobId; }
bool files::TransferService::start(TransferRequest request) { return launch(std::move(request), false); }
bool files::TransferService::retryFailed() {
  return m_state->job && retryable() > 0 ? launch(m_state->job->request, true) : false;
}
bool files::TransferService::launch(TransferRequest request, bool retry) {
  if (busy() || request.sources.empty()) {
    return false;
  }
  struct Request {
    std::shared_ptr<State> owner;
    TransferRequest transfer;
    std::shared_ptr<Job> job;
    bool retry;
  };
  m_state->active = true;
  m_state->action = request.action;
  m_state->total = request.sources.size();
  m_state->completed.store(0);
  m_state->bytes.store(0);
  m_state->cancelled.store(false);
  if (!retry) {
    ++m_state->jobId;
    m_state->results.clear();
    m_state->report.clear();
    m_state->retryable = 0;
    m_state->job.reset();
  }
  GioPtr<GTask> task(g_task_new(
      nullptr, nullptr,
      [](GObject*, GAsyncResult* result, gpointer) {
        auto* task = G_TASK(result);
        const auto owner = static_cast<Request*>(g_task_get_task_data(task))->owner;
        const auto& request = *static_cast<Request*>(g_task_get_task_data(task));
        const std::unique_ptr<std::vector<TransferResult>> outcomes(
            static_cast<std::vector<TransferResult>*>(g_task_propagate_pointer(task, nullptr))
        );
        owner->results = std::move(*outcomes);
        owner->job = request.job;
        owner->retryable = 0;
        for (const auto& node : owner->job->nodes) {
          if (node) {
            owner->retryable += retryCount(*node);
          }
        }
        owner->report = std::move(request.job->report);
        if (owner->retryable == 0) {
          owner->job.reset();
        }
        owner->active = false;
        const auto callback = owner->changed;
        if (callback) {
          callback();
        }
      },
      nullptr
  ));
  g_task_set_task_data(
      task.get(), new Request{m_state, std::move(request), retry ? m_state->job : nullptr, retry},
      [](gpointer data) { delete static_cast<Request*>(data); }
  );
  g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable*) {
    auto& request = *static_cast<Request*>(data);
    if (!request.job) {
      request.job = std::make_shared<Job>(request.transfer);
    }
    auto outcomes = std::make_unique<std::vector<TransferResult>>();
    for (const auto& source : request.transfer.sources) {
      outcomes->push_back({source, TransferOutcome::Failed, {}});
    }
    std::vector<bool> processed(outcomes->size(), false);
    std::map<std::pair<mode_t, mode_t>, TransferConflictChoice> policies;
    Work work{
        request.owner->cancelled, request.owner->bytes,
        [owner = request.owner, &policies, allowMerge = request.transfer.allowMerge](
            const Source& source, const Location& destination, const std::string& target, const struct stat& existing,
            bool forcePrompt
        ) {
          const auto kind = std::pair{source.metadata.st_mode & S_IFMT, existing.st_mode & S_IFMT};
          if (!forcePrompt) {
            if (const auto found = policies.find(kind); found != policies.end()) {
              return found->second;
            }
          }
          const files::GioPtr<GFile> child(g_file_get_child(destination.file().get(), target.c_str()));
          const files::GString uri(g_file_get_uri(child.get()));
          const files::GString name(g_filename_display_name(target.c_str()));
          std::unique_lock lock(owner->mutex);
          owner->choice.reset();
          owner->applyToAll = false;
          owner->conflict = TransferConflict{
              .id = ++owner->nextConflict,
              .source = source.location,
              .destination = Location::fromUri(uri.get()),
              .name = name.get(),
              .sourceDirectory = S_ISDIR(source.metadata.st_mode),
              .destinationDirectory = S_ISDIR(existing.st_mode),
              .sourceSize = S_ISDIR(source.metadata.st_mode) || source.metadata.st_size < 0
                  ? std::nullopt
                  : std::optional<std::uint64_t>(source.metadata.st_size),
              .destinationSize = S_ISDIR(existing.st_mode) || existing.st_size < 0
                  ? std::nullopt
                  : std::optional<std::uint64_t>(existing.st_size),
              .sourceModified = source.metadata.st_mtim.tv_sec < 0
                  ? std::nullopt
                  : std::optional<std::uint64_t>(source.metadata.st_mtim.tv_sec),
              .destinationModified =
                  existing.st_mtim.tv_sec < 0 ? std::nullopt : std::optional<std::uint64_t>(existing.st_mtim.tv_sec),
              .canMerge = allowMerge && S_ISDIR(source.metadata.st_mode) && S_ISDIR(existing.st_mode),
              .canApplyToAll = allowMerge,
              .scopeLabel = S_ISDIR(source.metadata.st_mode) && S_ISDIR(existing.st_mode)
                  ? "Apply to remaining folder conflicts in this job"
                  : "Apply to remaining conflicts of these types in this job"
          };
          lock.unlock();
          State::notify(owner);
          lock.lock();
          owner->decision.wait(lock, [&] { return owner->cancelled.load() || owner->choice.has_value(); });
          const auto choice = owner->cancelled.load() ? TransferConflictChoice::Cancel : *owner->choice;
          if (owner->applyToAll && choice != TransferConflictChoice::Cancel) {
            policies.insert_or_assign(kind, choice);
          }
          owner->conflict.reset();
          owner->choice.reset();
          return choice;
        }
    };
    work.allowMerge = request.transfer.allowMerge;
    work.changedLinks = request.job->changedLinks;
    work.mergedItems = request.job->mergedItems;
    if (request.retry) {
      *outcomes = request.job->outcomes;
      for (const auto& node : request.job->nodes) {
        if (node) {
          execute(*node, request.transfer.action, work, true);
          outcomes->at(node->source.index) = snapshot(*node);
        }
        request.owner->completed.fetch_add(1);
      }
      request.job->changedLinks = std::move(work.changedLinks);
      request.job->mergedItems = work.mergedItems;
      request.job->outcomes = *outcomes;
      request.job->report = jobReport(*outcomes);
      g_task_return_pointer(task, outcomes.release(), [](gpointer data) {
        delete static_cast<std::vector<TransferResult>*>(data);
      });
      return;
    }
    request.job->nodes.resize(outcomes->size());
    try {
      const auto path = request.transfer.destination.nativePath();
      if (!path) {
        throw std::runtime_error("Transfers to remote folders are not supported yet.");
      }
      auto destination = opened(::open(path->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      auto binding =
          std::make_shared<const Destination>(Destination{request.transfer.destination, info(destination.get()), {}});
      std::vector<Source> sources;
      for (std::size_t index = 0; index < request.transfer.sources.size(); ++index) {
        const auto& location = request.transfer.sources[index];
        try {
          work.check();
          const auto path = location.nativePath();
          if (!path) {
            throw std::runtime_error("Transfers from remote locations are not supported yet.");
          }
          const std::filesystem::path source(*path);
          const auto name = source.filename().native();
          if (name.empty() || name == "." || name == "..") {
            throw std::runtime_error("The filesystem root cannot be transferred.");
          }
          auto parent = opened(::open(source.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
          const auto metadata = info(parent.get(), name);
          sources.push_back({index, location, Fd{}, name, metadata, source.parent_path().native(), info(parent.get())});
        } catch (const TransferCancelled&) {
          (*outcomes)[index] = {location, TransferOutcome::Cancelled, "Transfer cancelled."};
          processed[index] = true;
          request.owner->completed.fetch_add(1);
        } catch (const std::exception& error) {
          (*outcomes)[index] = {location, TransferOutcome::Failed, error.what()};
          processed[index] = true;
          request.owner->completed.fetch_add(1);
        }
      }
      std::set<std::pair<dev_t, ino_t>> selectedDirectories;
      for (const auto& source : sources) {
        if (S_ISDIR(source.metadata.st_mode)) {
          selectedDirectories.emplace(source.metadata.st_dev, source.metadata.st_ino);
        }
      }
      std::set<std::tuple<dev_t, ino_t, std::string>> seen;
      std::vector<bool> included(sources.size(), false);
      // Classify overlaps before any selected parent is moved or removed.
      for (std::size_t i = 0; i < sources.size(); ++i) {
        auto& source = sources[i];
        try {
          work.check();
          source.parent = openSourceParent(source);
          const auto parentInfo = source.parentMetadata;
          included[i] = !seen.emplace(parentInfo.st_dev, parentInfo.st_ino, source.name).second;
          auto current = opened(::fcntl(source.parent.get(), F_DUPFD_CLOEXEC, 0));
          for (std::size_t depth = 0; !included[i] && !selectedDirectories.empty(); ++depth) {
            work.check();
            if (depth >= 65536) {
              throw std::runtime_error("The folder ancestry is too deep.");
            }
            const auto here = info(current.get());
            if (selectedDirectories.contains({here.st_dev, here.st_ino})) {
              included[i] = true;
              break;
            }
            auto parent = directory(current.get(), "..");
            if (sameIdentity(here, info(parent.get()))) {
              break;
            }
            current = std::move(parent);
          }
        } catch (const TransferCancelled&) {
          (*outcomes)[source.index] = {source.location, TransferOutcome::Cancelled, "Transfer cancelled."};
          processed[source.index] = true;
          request.owner->completed.fetch_add(1);
        } catch (const std::exception& error) {
          (*outcomes)[source.index] = {source.location, TransferOutcome::Failed, error.what()};
          processed[source.index] = true;
          request.owner->completed.fetch_add(1);
        }
        source.parent = Fd{};
      }

      for (std::size_t i = 0; i < sources.size(); ++i) {
        auto& source = sources[i];
        if (processed[source.index]) {
          continue;
        }
        try {
          if (included[i]) {
            (*outcomes)[source.index].outcome = TransferOutcome::Skipped;
          } else {
            Source captured{source.index,      source.location,      Fd{}, source.name, source.metadata,
                            source.parentPath, source.parentMetadata};
            auto node = std::make_shared<Node>(std::move(captured), binding, (*outcomes)[source.index]);
            request.job->nodes[source.index] = node;
            execute(*node, request.transfer.action, work, false);
            (*outcomes)[source.index] = snapshot(*node);
          }
        } catch (const std::exception& error) {
          (*outcomes)[source.index].error = error.what();
        }
        source.parent = Fd{};
        processed[source.index] = true;
        request.owner->completed.fetch_add(1);
      }

    } catch (const std::exception& error) {
      for (std::size_t i = 0; i < outcomes->size(); ++i) {
        if (!processed[i]) {
          (*outcomes)[i].error = error.what();
        }
      }
    }
    request.job->changedLinks = std::move(work.changedLinks);
    request.job->mergedItems = work.mergedItems;
    request.owner->completed.store(outcomes->size());
    request.job->outcomes = *outcomes;
    request.job->report = jobReport(*outcomes);
    g_task_return_pointer(task, outcomes.release(), [](gpointer data) {
      delete static_cast<std::vector<TransferResult>*>(data);
    });
  });
  const auto callback = m_state->changed;
  if (callback) {
    callback();
  }
  return true;
}
