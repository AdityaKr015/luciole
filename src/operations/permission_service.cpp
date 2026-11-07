#include "operations/permission_service.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <limits>
#include <pwd.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace {
  struct Fd {
    int value;
    explicit Fd(int value) : value(value) {
      if (value < 0) {
        throw std::runtime_error(std::strerror(errno));
      }
    }
    ~Fd() { ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
  };
  struct stat metadata(int fd) {
    struct stat value{};
    if (::fstat(fd, &value) != 0) {
      throw std::runtime_error(std::strerror(errno));
    }
    return value;
  }
  bool same(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && (a.st_mode & S_IFMT) == (b.st_mode & S_IFMT);
  }
  bool samePermissions(const struct stat& a, const struct stat& b) {
    return a.st_uid == b.st_uid && a.st_gid == b.st_gid && (a.st_mode & 07777) == (b.st_mode & 07777);
  }
  template <typename Record, typename Lookup> std::optional<Record> account(std::vector<char>& buffer, Lookup lookup) {
    for (std::size_t size = 4096; size <= 1024 * 1024; size *= 2) {
      buffer.resize(size);
      Record record{}, *result = nullptr;
      const int code = lookup(record, buffer, result);
      if (code == ERANGE) {
        continue;
      }
      if (code != 0) {
        throw std::runtime_error("Account lookup failed: " + std::string(std::strerror(code)));
      }
      if (result) {
        return record;
      }
      return std::nullopt;
    }
    throw std::runtime_error("Account lookup exceeded its size limit.");
  }
  std::string accountName(std::uint32_t id, bool user) {
    std::vector<char> buffer;
    if (user) {
      const auto record = account<passwd>(buffer, [id](auto& record, auto& bytes, auto*& result) {
        return ::getpwuid_r(id, &record, bytes.data(), bytes.size(), &result);
      });
      if (record) {
        return record->pw_name;
      }
    } else {
      const auto record = account<group>(buffer, [id](auto& record, auto& bytes, auto*& result) {
        return ::getgrgid_r(id, &record, bytes.data(), bytes.size(), &result);
      });
      if (record) {
        return record->gr_name;
      }
    }
    return {};
  }
  std::uint32_t accountId(const std::string& text, bool user) {
    if (text.empty() || text.size() > 1024 || text.contains('\0')) {
      throw std::runtime_error("Enter an account name or a numeric ID.");
    }
    if (std::ranges::all_of(text, [](unsigned char c) { return c >= '0' && c <= '9'; })) {
      std::uint32_t value = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      if (parsed.ec != std::errc{} || value == std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("Account ID is out of range.");
      }
      return value;
    }
    std::vector<char> buffer;
    if (user) {
      const auto record = account<passwd>(buffer, [&text](auto& record, auto& bytes, auto*& result) {
        return ::getpwnam_r(text.c_str(), &record, bytes.data(), bytes.size(), &result);
      });
      if (record && record->pw_uid != static_cast<uid_t>(-1)) {
        return record->pw_uid;
      }
    } else {
      const auto record = account<group>(buffer, [&text](auto& record, auto& bytes, auto*& result) {
        return ::getgrnam_r(text.c_str(), &record, bytes.data(), bytes.size(), &result);
      });
      if (record && record->gr_gid != static_cast<gid_t>(-1)) {
        return record->gr_gid;
      }
    }
    throw std::runtime_error(user ? "Owner account was not found." : "Group was not found.");
  }
  bool member(gid_t group) {
    if (group == ::getegid()) {
      return true;
    }
    const int count = ::getgroups(0, nullptr);
    if (count < 0) {
      throw std::runtime_error(std::strerror(errno));
    }
    std::vector<gid_t> groups(static_cast<std::size_t>(count));
    if (::getgroups(count, groups.data()) < 0) {
      throw std::runtime_error(std::strerror(errno));
    }
    return std::ranges::find(groups, group) != groups.end();
  }
  struct Target {
    files::DirectoryEntry entry;
    std::string path;
    Fd fd;
    struct stat baseline;
    bool identified = false;
    explicit Target(files::DirectoryEntry entry)
        : entry(std::move(entry)), path(*this->entry.location.nativePath()),
          fd(::open(path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC)), baseline(metadata(fd.value)) {}
    bool current() const {
      struct stat value{};
      return ::lstat(path.c_str(), &value) == 0 && same(value, baseline);
    }
    void validate() const {
      if (!identified || !current()) {
        throw std::runtime_error("The item moved or was replaced. Reopen Properties.");
      }
      if (!samePermissions(metadata(fd.value), baseline)) {
        throw std::runtime_error("Permissions or ownership changed elsewhere. Reopen Properties.");
      }
    }
    files::PermissionInfo inspect() const {
      const auto value = metadata(fd.value);
      const bool root = ::geteuid() == 0, owned = value.st_uid == ::geteuid();
      struct statvfs filesystem{};
      const bool readOnly = ::statvfs(("/proc/self/fd/" + std::to_string(fd.value)).c_str(), &filesystem) == 0
          && (filesystem.f_flag & ST_RDONLY) != 0;
      const bool valid = current(), editable = identified && valid && !readOnly;
      files::PermissionInfo info{
          .mode = value.st_mode & 07777,
          .owner = value.st_uid,
          .group = value.st_gid,
          .ownerName = {},
          .groupName = {},
          .canMode = editable && (root || owned) && !S_ISLNK(value.st_mode),
          .canOwner = editable && root,
          .canGroup = editable && (root || owned),
          .symlink = S_ISLNK(value.st_mode),
          .multipleLinks = S_ISREG(value.st_mode) && value.st_nlink > 1,
          .current = valid,
          .reason = {}
      };
      // Display names are optional; unavailable account services must not hide known IDs/mode.
      try {
        info.ownerName = accountName(info.owner, true);
      } catch (const std::exception&) {
      }
      try {
        info.groupName = accountName(info.group, false);
      } catch (const std::exception&) {
      }
      if (!valid) {
        info.reason = "The item moved or was replaced. Reopen Properties.";
      } else if (!identified) {
        info.reason = "This provider does not identify the captured item; editing is unavailable.";
      } else if (readOnly) {
        info.reason = "This filesystem is read-only.";
      } else if (!root && !owned) {
        info.reason = "Only the owner or an administrator can change these permissions.";
      } else if (info.symlink) {
        info.reason = "Link permissions cannot be edited. Ownership applies to the link itself.";
      }
      return info;
    }
  };
  struct Reply {
    std::shared_ptr<Target> target;
    std::optional<files::PermissionInfo> info;
    std::string message;
    bool error = false, applied = false;
  };
  Reply apply(const std::shared_ptr<Target>& target, const files::PermissionEdit& edit, const std::atomic<bool>& stop) {
    Reply reply{.target = target, .info = {}, .message = {}};
    const auto before = metadata(target->fd.value);
    bool mutated = false;
    try {
      target->validate();
      const auto info = target->inspect();
      const uid_t owner = edit.owner ? accountId(*edit.owner, true) : before.st_uid;
      const gid_t group = edit.group ? accountId(*edit.group, false) : before.st_gid;
      if (edit.mode && (*edit.mode & ~07777U) != 0) {
        throw std::runtime_error("Permission bits are out of range.");
      }
      if (edit.mode && (info.symlink || (*edit.mode != (before.st_mode & 07777) && !info.canMode))) {
        throw std::runtime_error(
            info.symlink ? "Link permissions cannot be edited." : "Permission changes are not permitted."
        );
      }
      if ((owner != before.st_uid && !info.canOwner) || (group != before.st_gid && !info.canGroup)) {
        throw std::runtime_error("Ownership changes are not permitted.");
      }
      if (group != before.st_gid && ::geteuid() != 0 && !member(group)) {
        throw std::runtime_error("You must be a member of the requested group.");
      }
      target->validate();
      if (!stop.load() && (owner != before.st_uid || group != before.st_gid)) {
        if (::fchownat(
                target->fd.value, "", owner == before.st_uid ? static_cast<uid_t>(-1) : owner,
                group == before.st_gid ? static_cast<gid_t>(-1) : group, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW
            )
            != 0) {
          throw std::runtime_error(std::strerror(errno));
        }
        target->baseline = metadata(target->fd.value);
        mutated = true;
      }
      if (!stop.load() && edit.mode && *edit.mode != (metadata(target->fd.value).st_mode & 07777)) {
        target->validate();
        // The procfs magic link addresses the held non-link inode, never a replacement pathname.
        if (::chmod(("/proc/self/fd/" + std::to_string(target->fd.value)).c_str(), *edit.mode) != 0) {
          throw std::runtime_error(std::strerror(errno));
        }
        mutated = true;
      }
      const auto after = metadata(target->fd.value);
      reply.applied = mutated && !samePermissions(before, after);
      if (!stop.load()
          && (after.st_uid != owner || after.st_gid != group || (edit.mode && (after.st_mode & 07777) != *edit.mode))) {
        throw std::runtime_error("The filesystem did not retain all requested permissions or ownership.");
      }
      reply.message = stop.load() ? "Cancelled. Completed changes remain applied."
          : reply.applied         ? "Changes saved."
                                  : "No changes.";
      target->baseline = after;
    } catch (const std::exception& error) {
      reply.applied = mutated && !samePermissions(before, metadata(target->fd.value));
      reply.error = true;
      reply.message = (reply.applied ? "Some changes were applied. " : "") + std::string(error.what());
      // Stale edits require reopening; do not silently adopt another writer's baseline.
    }
    reply.info = target->inspect();
    if (!samePermissions(metadata(target->fd.value), target->baseline)) {
      reply.info->canMode = reply.info->canOwner = reply.info->canGroup = false;
      reply.info->reason = "Permissions or ownership changed elsewhere. Reopen Properties.";
    }
    return reply;
  }
} // namespace

struct files::PermissionService::State {
  PermissionSnapshot snapshot;
  std::shared_ptr<Target> target;
  std::atomic<bool> stop{false};
  bool active = false;
  std::function<void()> changed;
  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  static void launch(const std::shared_ptr<State>& owner, std::function<Reply()> work, bool saving) {
    struct Request {
      std::shared_ptr<State> owner;
      std::function<Reply()> work;
      bool saving;
    };
    owner->active = true;
    owner->stop.store(false);
    const GioPtr<GTask> task(g_task_new(
        nullptr, nullptr,
        [](GObject*, GAsyncResult* result, gpointer) {
          const auto& request = *static_cast<Request*>(g_task_get_task_data(G_TASK(result)));
          const auto owner = request.owner;
          const std::unique_ptr<Reply> reply(static_cast<Reply*>(g_task_propagate_pointer(G_TASK(result), nullptr)));
          owner->active = false;
          owner->snapshot.info = reply->info;
          owner->snapshot.message = reply->message;
          owner->snapshot.error = reply->error;
          owner->snapshot.applied = reply->applied;
          owner->target = reply->target;
          owner->snapshot.phase = owner->stop.load() && !request.saving ? PermissionPhase::Cancelled
              : reply->info                                             ? PermissionPhase::Ready
                                                                        : PermissionPhase::Error;
          if (owner->stop.load()) {
            owner->target.reset();
          }
          owner->notify();
        },
        nullptr
    ));
    g_task_set_task_data(task.get(), new Request{owner, std::move(work), saving}, [](gpointer p) {
      delete static_cast<Request*>(p);
    });
    g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable*) {
      const auto& request = *static_cast<Request*>(data);
      auto reply = std::make_unique<Reply>();
      try {
        *reply = request.work();
      } catch (const std::exception& error) {
        reply->error = true;
        reply->message = error.what();
      }
      g_task_return_pointer(task, reply.release(), [](gpointer p) { delete static_cast<Reply*>(p); });
    });
    owner->notify();
  }
};
files::PermissionService::PermissionService() : m_state(std::make_shared<State>()) {}
files::PermissionService::~PermissionService() {
  m_state->changed = {};
  cancel();
}
bool files::PermissionService::busy() const { return m_state->active; }
const files::PermissionSnapshot& files::PermissionService::snapshot() const { return m_state->snapshot; }
void files::PermissionService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
void files::PermissionService::cancel() {
  m_state->stop.store(true);
  if (!busy()) {
    m_state->target.reset();
  }
}
bool files::PermissionService::prepare(DirectoryEntry entry) { return prepareEntry(std::move(entry), false); }
bool files::PermissionService::prepare(const Location& location) {
  return prepareEntry(
      {.location = location,
       .name = {},
       .displayName = {},
       .size = {},
       .modifiedSeconds = {},
       .etag = {},
       .parentDisplayName = {}},
      true
  );
}
bool files::PermissionService::prepareEntry(DirectoryEntry entry, bool captureCurrentLocation) {
  if (busy()) {
    return false;
  }
  m_state->target.reset();
  m_state->snapshot = {};
  m_state->snapshot.phase = PermissionPhase::Loading;
  State::launch(
      m_state,
      [owner = m_state, entry = std::move(entry), captureCurrentLocation] {
        if (!entry.location.nativePath() || entry.location.isTrash()) {
          throw std::runtime_error("Permission editing is available for native items outside Trash.");
        }
        auto target = std::make_shared<Target>(entry);
        const GioPtr<GFileInfo> info(g_file_query_info(
            entry.location.file().get(), "id::file,id::filesystem,unix::device,unix::inode",
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
        ));
        if (!info || !target->current() || (entry.identity && readFileIdentity(info.get()) != entry.identity)) {
          throw std::runtime_error("The item changed. Reopen Properties.");
        }
        if (g_file_info_has_attribute(info.get(), "unix::device")
            && g_file_info_has_attribute(info.get(), "unix::inode")
            && (g_file_info_get_attribute_uint32(info.get(), "unix::device") != target->baseline.st_dev
                || g_file_info_get_attribute_uint64(info.get(), "unix::inode") != target->baseline.st_ino)) {
          throw std::runtime_error("The item changed. Reopen Properties.");
        }
        target->identified =
            entry.identity.has_value() || (captureCurrentLocation && readFileIdentity(info.get()).has_value());
        if (owner->stop.load()) {
          return Reply{};
        }
        return Reply{.target = target, .info = target->inspect(), .message = {}};
      },
      false
  );
  return true;
}
bool files::PermissionService::apply(PermissionEdit edit) {
  if (busy() || !m_state->target || m_state->snapshot.phase != PermissionPhase::Ready) {
    return false;
  }
  const auto& info = m_state->snapshot.info;
  if (!info || (!info->canMode && !info->canOwner && !info->canGroup)) {
    return false;
  }
  m_state->snapshot.phase = PermissionPhase::Saving;
  m_state->snapshot.message.clear();
  m_state->snapshot.error = m_state->snapshot.applied = false;
  State::launch(
      m_state,
      [owner = m_state, target = m_state->target, edit = std::move(edit)] {
        return ::apply(target, edit, owner->stop);
      },
      true
  );
  return true;
}
