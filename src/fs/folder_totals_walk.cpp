#include "fs/folder_totals_walk.h"

#include "fs/file_query.h"
#include "fs/gio_interop.h"
#include "fs/posix_fd.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <map>
#include <set>
#include <unistd.h>

namespace {
  struct CloseDirectory {
    void operator()(DIR* value) const { ::closedir(value); }
  };
  bool sameStamp(const struct stat& a, const struct stat& b) {
    return files::sameIdentity(a, b)
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec
        && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec
        && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec
        && a.st_size == b.st_size;
  }
  class Walker {
  public:
    Walker(
        std::vector<files::DirectoryEntry> roots, const files::CancelToken& token,
        const std::function<void(const files::FolderTotalsSnapshot&)>& progress, files::FolderTotalsLimits limits
    )
        : m_roots(std::move(roots)), m_token(token), m_progress(progress), m_limits(limits) {
      std::ranges::stable_sort(m_roots, {}, [](const auto& entry) { return entry.location.uri().size(); });
      for (const auto& root : m_roots) {
        m_selected.emplace(root.location.uri(), &root);
      }
      m_result.phase = files::FolderTotalsPhase::Running;
    }
    files::FolderTotalsSnapshot run() {
      m_progress(m_result);
      for (const auto& root : m_roots) {
        if (stopped()) {
          break;
        }
        if (m_seenRoots.contains(root.location.uri())) {
          continue;
        }
        try {
          if (!root.location.isNative() || root.location.isTrash()) {
            ++m_result.unsupported;
            continue;
          }
          const auto fd =
              files::checkedFd(::open(root.location.nativePath()->c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
          const auto value = files::statFd(fd.get());
          visit(fd.get(), value, root.location, 0);
          struct stat current{};
          if (::lstat(root.location.nativePath()->c_str(), &current) != 0 || !sameStamp(value, current)) {
            ++m_result.changed;
          }
        } catch (const files::FileErrorException& error) {
          failed(error.error());
        }
      }
      m_result.phase = m_token.cancelled() ? files::FolderTotalsPhase::Cancelled : files::FolderTotalsPhase::Complete;
      m_progress(m_result);
      return m_result;
    }

  private:
    bool stopped() {
      if (m_token.cancelled()) {
        return true;
      }
      if (m_items >= m_limits.items) {
        m_result.limited = true;
        return true;
      }
      return false;
    }
    void failed(const files::FileError& error) {
      if (error.kind == files::FileErrorKind::Cancelled) {
        return;
      }
      if (error.kind == files::FileErrorKind::Changed) {
        ++m_result.changed;
      } else {
        ++m_result.unavailable;
      }
      // Retain only one bounded error regardless of the tree size.
      m_result.error = error.message.substr(0, 256);
    }
    void validateRoot(const files::DirectoryEntry& root, const struct stat& value) const {
      const auto details = files::readFileDetails(root.location, m_token);
      const files::GioPtr<GFileInfo> info(g_file_query_info(
          files::gioFile(root.location).get(), "unix::device,unix::inode", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
          files::gioCancellable(m_token), nullptr
      ));
      if (m_token.cancelled()) {
        files::throwFileError(files::FileErrorKind::Cancelled, "Cancelled.");
      }
      if (!info
          || !g_file_info_has_attribute(info.get(), "unix::device")
          || !g_file_info_has_attribute(info.get(), "unix::inode")
          || g_file_info_get_attribute_uint32(info.get(), "unix::device") != value.st_dev
          || g_file_info_get_attribute_uint64(info.get(), "unix::inode") != value.st_ino
          || (root.identity && root.identity != details.identity)
          || (root.etag && root.etag != details.etag)
          || (root.contentRevision && root.contentRevision != details.contentRevision)) {
        files::throwFileError(files::FileErrorKind::Changed, "A selected item changed. Reopen Properties.");
      }
    }
    void visit(int fd, const struct stat& value, const files::Location& location, std::size_t depth) {
      if (stopped()) {
        return;
      }
      if (const auto found = m_selected.find(location.uri()); found != m_selected.end()) {
        if (!m_seenRoots.insert(location.uri()).second) {
          return;
        }
        validateRoot(*found->second, value);
      }
      ++m_items;
      if (S_ISDIR(value.st_mode)) {
        if (!m_directories.emplace(value.st_dev, value.st_ino).second) {
          ++m_result.unavailable; // Directory alias/cycle: no duplicate subtree totals.
        } else {
          m_result.folders += depth != 0; // Contents exclude the selected root folders.
          if (depth >= m_limits.depth) {
            m_result.limited = true;
          } else {
            children(fd, value, location, depth);
          }
        }
      } else {
        if (S_ISLNK(value.st_mode)) {
          ++m_result.links;
        } else if (S_ISREG(value.st_mode)) {
          ++m_result.files;
        } else {
          ++m_result.other;
        }
        // Special files have no meaningful content length and are never opened.
        if (S_ISREG(value.st_mode) || S_ISLNK(value.st_mode)) {
          if (value.st_size < 0) {
            ++m_result.unavailable;
          } else if (
              static_cast<std::uint64_t>(value.st_size) > std::numeric_limits<std::uint64_t>::max() - m_result.bytes
          ) {
            m_result.overflow = true;
          } else {
            m_result.bytes += static_cast<std::uint64_t>(value.st_size);
          }
        }
      }
      if (m_items % 64 == 0) {
        m_progress(m_result);
      }
    }
    void children(int fd, const struct stat& baseline, const files::Location& location, std::size_t depth) {
      const int descriptor = ::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (descriptor < 0) {
        files::throwErrno(errno);
      }
      const std::unique_ptr<DIR, CloseDirectory> directory(::fdopendir(descriptor));
      if (!directory) {
        const int code = errno;
        ::close(descriptor);
        files::throwErrno(code);
      }
      while (!stopped()) {
        errno = 0;
        const auto* item = ::readdir(directory.get());
        if (!item) {
          if (errno) {
            failed(files::errnoError(errno));
          }
          break;
        }
        const std::string name(item->d_name);
        if (name == "." || name == "..") {
          continue;
        }
        try {
          const auto child =
              files::checkedFd(::openat(::dirfd(directory.get()), name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
          const auto value = files::statFd(child.get());
          visit(child.get(), value, location.child(name), depth + 1);
          struct stat current{};
          if (::fstatat(::dirfd(directory.get()), name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0
              || !sameStamp(value, current)) {
            ++m_result.changed;
          }
        } catch (const files::FileErrorException& error) {
          failed(error.error());
        }
      }
      if (!sameStamp(baseline, files::statFd(fd))) {
        ++m_result.changed;
      }
    }
    std::vector<files::DirectoryEntry> m_roots;
    const files::CancelToken& m_token;
    const std::function<void(const files::FolderTotalsSnapshot&)>& m_progress;
    files::FolderTotalsLimits m_limits;
    files::FolderTotalsSnapshot m_result;
    std::size_t m_items = 0;
    std::map<std::string, const files::DirectoryEntry*> m_selected;
    std::set<std::string> m_seenRoots;
    std::set<std::pair<dev_t, ino_t>> m_directories;
  };
} // namespace

files::FolderTotalsSnapshot files::walkFolderTotals(
    std::vector<DirectoryEntry> roots, const CancelToken& token,
    const std::function<void(const FolderTotalsSnapshot&)>& progress, FolderTotalsLimits limits
) {
  return Walker(std::move(roots), token, progress, limits).run();
}
