#include "operations/permission_walk.h"

#include "fs/filename.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  using files::PermissionOutcome;
  using files::PermissionTarget;

  constexpr std::size_t maxItems = 16384;
  constexpr std::size_t maxDepth = 128;

  // Collection is complete before mutations begin. The item and depth limits also
  // bound retained descriptors, result memory and the traversal stack.
  class CollectionLimit final : public files::FileErrorException {
  public:
    using files::FileErrorException::FileErrorException;
  };
  [[noreturn]] void limit(files::FileErrorKind kind, const char* message) { throw CollectionLimit({kind, message}); }
  [[noreturn]] void descriptorLimit(int code) {
    limit(files::errnoError(code).kind, "Not enough file descriptors to collect this job. Select smaller folders.");
  }
  [[noreturn]] void throwErrnoWithContext(int code, const std::string& context) {
    auto error = files::errnoError(code);
    files::throwFileError(error.kind, context + error.message);
  }
  struct CloseDirectory {
    void operator()(DIR* directory) const { ::closedir(directory); }
  };
  std::uint64_t mountId(int fd) {
    struct statx value{};
    if (::statx(fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &value) != 0
        || (value.stx_mask & STATX_MNT_ID) == 0) {
      files::throwFileError(files::FileErrorKind::Unsupported, "Cannot identify the filesystem mount boundary.");
    }
    return value.stx_mnt_id;
  }

  class Walker {
  public:
    Walker(
        const files::PermissionBatch& roots, files::PermissionBatch& plan, const files::CancelToken& token,
        std::atomic<std::size_t>& found
    )
        : m_roots(roots), m_plan(plan), m_token(token), m_found(found) {
      for (std::size_t i = 0; i < roots.results->size(); ++i) {
        m_selected.emplace((*roots.results)[i].location.uri(), i);
      }
    }

    void run() {
      std::vector<std::size_t> order;
      order.reserve(m_roots.targets.size());
      for (std::size_t i = 0; i < m_roots.targets.size(); ++i) {
        order.push_back(i);
      }
      // An explicitly selected descendant is reused by its ancestor's walk.
      // Sorting roots by path length ensures ancestors are visited first.
      std::ranges::stable_sort(order, {}, [&](std::size_t index) {
        return (*m_roots.results)[index].location.uri().size();
      });
      for (const auto i : order) {
        if (m_token.cancelled()) {
          break;
        }
        const auto& target = m_roots.targets[i];
        if (target) {
          walk(target, mountId(target->fd.get()), 0);
        } else if (m_seen.insert((*m_roots.results)[i].location.uri()).second) {
          append((*m_roots.results)[i], {});
        }
      }
    }

  private:
    void append(const files::PermissionResult& row, std::shared_ptr<PermissionTarget> target) {
      if (m_plan.results->size() >= maxItems) {
        limit(files::FileErrorKind::Unsupported, "The recursive job exceeds 16384 items. Select smaller folders.");
      }
      m_plan.results->push_back(row);
      m_plan.targets.push_back(std::move(target));
    }

    void skip(files::PermissionResult& row, std::shared_ptr<PermissionTarget> target, std::string message) {
      row.outcome = PermissionOutcome::Skipped;
      row.message = std::move(message);
      append(row, std::move(target));
    }

    void walk(std::shared_ptr<PermissionTarget> target, std::uint64_t rootMount, std::size_t depth) {
      if (m_token.cancelled() || !m_seen.insert(target->entry.location.uri()).second) {
        return;
      }
      if (m_found.fetch_add(1) >= maxItems) {
        limit(files::FileErrorKind::Unsupported, "The recursive job exceeds 16384 items. Select smaller folders.");
      }
      files::PermissionResult row{
          .location = target->entry.location,
          .name = target->entry.displayName,
          .info = {},
          .outcome = PermissionOutcome::Ready,
          .error = {},
          .message = {}
      };
      try {
        target->validate();
        row.info = target->inspect(false);
        if (S_ISLNK(target->baseline.st_mode)) {
          skip(row, std::move(target), "Symbolic link skipped; its target was not followed.");
          return;
        }
        if (mountId(target->fd.get()) != rootMount) {
          skip(row, std::move(target), "Mounted subtree skipped.");
          return;
        }
        if (S_ISDIR(target->baseline.st_mode)) {
          if (depth >= maxDepth) {
            limit(
                files::FileErrorKind::Unsupported,
                "The recursive job exceeds 128 folder levels. Select smaller folders."
            );
          }
          if (!m_directories.emplace(target->baseline.st_dev, target->baseline.st_ino).second) {
            skip(row, std::move(target), "Folder already visited; subtree skipped.");
            return;
          }
          walkChildren(target, rootMount, depth);
        }
        // Postorder keeps parent access intact while processing descendants.
        append(row, std::move(target));
      } catch (const CollectionLimit&) {
        throw;
      } catch (const std::exception&) {
        row.outcome = PermissionOutcome::Failed;
        row.error = files::currentFileError();
        append(row, {});
      }
    }

    void walkChildren(const std::shared_ptr<PermissionTarget>& target, std::uint64_t rootMount, std::size_t depth) {
      const int descriptor = ::openat(target->fd.get(), ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (descriptor < 0) {
        if (errno == EMFILE || errno == ENFILE) {
          descriptorLimit(errno);
        }
        throwErrnoWithContext(errno, "Contents not inspected: ");
      }
      // fdopendir owns the descriptor only on success.
      const std::unique_ptr<DIR, CloseDirectory> directory(::fdopendir(descriptor));
      if (!directory) {
        const int code = errno;
        ::close(descriptor);
        files::throwErrno(code);
      }
      while (!m_token.cancelled()) {
        errno = 0;
        const auto* child = ::readdir(directory.get());
        if (!child) {
          if (errno != 0) {
            throwErrnoWithContext(errno, "Contents only partly inspected: ");
          }
          break;
        }
        const std::string name(child->d_name);
        if (name == "." || name == "..") {
          continue;
        }
        target->validate();
        auto entry = target->entry;
        entry.location = target->entry.location.child(name);
        entry.name = name;
        entry.displayName = files::filenameDisplayText(name);
        if (const auto found = m_selected.find(entry.location.uri()); found != m_selected.end()) {
          const auto& captured = m_roots.targets[found->second];
          if (captured) {
            captured->ancestors = target->ancestors;
            captured->ancestors.push_back(target);
            walk(captured, mountId(captured->fd.get()), depth + 1);
          } else if (m_seen.insert(entry.location.uri()).second) {
            append((*m_roots.results)[found->second], {});
          }
          continue;
        }
        if (m_seen.contains(entry.location.uri())) {
          continue;
        }
        const int fd = ::openat(::dirfd(directory.get()), name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
          if (errno == EMFILE || errno == ENFILE) {
            descriptorLimit(errno);
          }
          append(
              {.location = entry.location,
               .name = entry.displayName,
               .info = {},
               .outcome = PermissionOutcome::Failed,
               .error = files::errnoError(errno),
               .message = {}},
              {}
          );
          continue;
        }
        auto ancestors = target->ancestors;
        ancestors.push_back(target);
        walk(std::make_shared<PermissionTarget>(std::move(entry), fd, std::move(ancestors)), rootMount, depth + 1);
      }
    }

    const files::PermissionBatch& m_roots;
    files::PermissionBatch& m_plan;
    const files::CancelToken& m_token;
    std::atomic<std::size_t>& m_found;
    std::map<std::string, std::size_t> m_selected;
    std::set<std::string> m_seen;
    std::set<std::pair<dev_t, ino_t>> m_directories;
  };
} // namespace

void files::collectPermissionTargets(
    const PermissionBatch& roots, PermissionBatch& plan, const CancelToken& token, std::atomic<std::size_t>& found
) {
  Walker(roots, plan, token, found).run();
}
