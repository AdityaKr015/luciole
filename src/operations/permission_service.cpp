#include "operations/permission_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <format>
#include <grp.h>
#include <limits>
#include <map>
#include <pwd.h>
#include <set>
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
    std::vector<std::shared_ptr<Target>> ancestors;
    explicit Target(files::DirectoryEntry entry)
        : entry(std::move(entry)), path(*this->entry.location.nativePath()),
          fd(::open(path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC)), baseline(metadata(fd.value)) {}
    Target(files::DirectoryEntry entry, int descriptor, std::vector<std::shared_ptr<Target>> ancestors)
        : entry(std::move(entry)), path(*this->entry.location.nativePath()), fd(descriptor),
          baseline(metadata(fd.value)), identified(true), ancestors(std::move(ancestors)) {
      this->entry.type = S_ISDIR(baseline.st_mode) ? G_FILE_TYPE_DIRECTORY
          : S_ISLNK(baseline.st_mode)              ? G_FILE_TYPE_SYMBOLIC_LINK
          : S_ISREG(baseline.st_mode)              ? G_FILE_TYPE_REGULAR
                                                   : G_FILE_TYPE_SPECIAL;
      this->entry.symlink = S_ISLNK(baseline.st_mode);
      this->entry.identity.reset(); // Identified directly by the held native descriptor.
    }
    bool current() const {
      struct stat value{};
      return ::lstat(path.c_str(), &value) == 0 && same(value, baseline);
    }
    void validate() const {
      for (const auto& ancestor : ancestors) {
        if (!ancestor->current() || !samePermissions(metadata(ancestor->fd.value), ancestor->baseline)) {
          throw std::runtime_error("A containing folder changed. Reopen Properties.");
        }
      }
      if (!identified) {
        throw std::runtime_error("The captured item has no provider identity; editing is unavailable.");
      }
      if (!current()) {
        throw std::runtime_error("The item moved or was replaced. Reopen Properties.");
      }
      if (!samePermissions(metadata(fd.value), baseline)) {
        throw std::runtime_error("Permissions or ownership changed elsewhere. Reopen Properties.");
      }
    }
    files::PermissionInfo inspect(bool names = true) const {
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
          .directory = S_ISDIR(value.st_mode),
          .reason = {}
      };
      // Display names are optional; unavailable account services must not hide known IDs/mode.
      try {
        if (names) {
          info.ownerName = accountName(info.owner, true);
        }
      } catch (const std::exception&) {
      }
      try {
        if (names) {
          info.groupName = accountName(info.group, false);
        }
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
  std::shared_ptr<Target> prepareTarget(const files::DirectoryEntry& entry, bool captureCurrentLocation) {
    if (!entry.location.nativePath() || entry.location.isTrash()) {
      throw std::runtime_error("Permission editing is available for native items outside Trash.");
    }
    auto target = std::make_shared<Target>(entry);
    const files::GioPtr<GFileInfo> info(g_file_query_info(
        entry.location.file().get(), "id::file,id::filesystem,unix::device,unix::inode",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    if (!info || !target->current() || (entry.identity && files::readFileIdentity(info.get()) != entry.identity)) {
      throw std::runtime_error("The item changed. Reopen Properties.");
    }
    if (g_file_info_has_attribute(info.get(), "unix::device")
        && g_file_info_has_attribute(info.get(), "unix::inode")
        && (g_file_info_get_attribute_uint32(info.get(), "unix::device") != target->baseline.st_dev
            || g_file_info_get_attribute_uint64(info.get(), "unix::inode") != target->baseline.st_ino)) {
      throw std::runtime_error("The item changed. Reopen Properties.");
    }
    target->identified =
        entry.identity.has_value() || (captureCurrentLocation && files::readFileIdentity(info.get()).has_value());
    return target;
  }
  struct Bulk {
    std::vector<std::shared_ptr<Target>> targets;
    std::shared_ptr<std::vector<files::PermissionResult>> results;
  };
  struct Reply {
    std::shared_ptr<Target> target;
    std::optional<files::PermissionInfo> info;
    std::string message;
    bool error = false, applied = false;
    std::shared_ptr<Bulk> bulk;
    std::shared_ptr<std::vector<files::PermissionResult>> results;
    bool finished = false;
    std::string report;
  };
  Reply apply(
      const std::shared_ptr<Target>& target, const files::PermissionEdit& edit, const std::atomic<bool>& stop,
      bool names = true
  ) {
    Reply reply{.target = target, .info = {}, .message = {}, .bulk = {}, .results = {}, .report = {}};
    const auto before = metadata(target->fd.value);
    bool mutated = false;
    try {
      target->validate();
      const auto info = target->inspect(names);
      const uid_t owner = edit.owner ? accountId(*edit.owner, true) : before.st_uid;
      const gid_t group = edit.group ? accountId(*edit.group, false) : before.st_gid;
      if ((edit.mask & ~07777U) != 0 || (edit.mode && (*edit.mode & ~07777U) != 0)) {
        throw std::runtime_error("Permission bits are out of range.");
      }
      if (edit.mode && (info.symlink || (((*edit.mode ^ before.st_mode) & edit.mask) != 0 && !info.canMode))) {
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
      if (!stop.load() && edit.mode && ((*edit.mode ^ metadata(target->fd.value).st_mode) & edit.mask) != 0) {
        target->validate();
        const auto desired = (metadata(target->fd.value).st_mode & 07777 & ~edit.mask) | (*edit.mode & edit.mask);
        // The procfs magic link addresses the held non-link inode, never a replacement pathname.
        if (::chmod(("/proc/self/fd/" + std::to_string(target->fd.value)).c_str(), desired) != 0) {
          throw std::runtime_error(std::strerror(errno));
        }
        mutated = true;
      }
      const auto after = metadata(target->fd.value);
      reply.applied = mutated && !samePermissions(before, after);
      if (!stop.load()
          && (after.st_uid != owner
              || after.st_gid != group
              || (edit.mode && ((after.st_mode ^ *edit.mode) & edit.mask) != 0))) {
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
    reply.info = target->inspect(names);
    if (!samePermissions(metadata(target->fd.value), target->baseline)) {
      reply.info->canMode = reply.info->canOwner = reply.info->canGroup = false;
      reply.info->reason = "Permissions or ownership changed elsewhere. Reopen Properties.";
    }
    return reply;
  }
  void summarize(files::PermissionSnapshot& snapshot, const std::vector<files::PermissionResult>& rows) {
    snapshot.info.reset();
    snapshot.mixedMode = 0;
    snapshot.mixedOwner = snapshot.mixedGroup = false;
    snapshot.unknownItems = 0;
    for (const auto& row : rows) {
      if (!row.info) {
        ++snapshot.unknownItems;
        continue;
      }
      const auto& info = *row.info;
      if (!snapshot.info) {
        snapshot.info = info;
        snapshot.info->reason.clear();
        continue;
      }
      auto& summary = *snapshot.info;
      snapshot.mixedMode |= summary.mode ^ info.mode;
      snapshot.mixedOwner = snapshot.mixedOwner || summary.owner != info.owner;
      snapshot.mixedGroup = snapshot.mixedGroup || summary.group != info.group;
      summary.canMode = summary.canMode || info.canMode;
      summary.canOwner = summary.canOwner || info.canOwner;
      summary.canGroup = summary.canGroup || info.canGroup;
      summary.multipleLinks = summary.multipleLinks || info.multipleLinks;
      summary.symlink = summary.symlink || info.symlink;
    }
    if (snapshot.info && snapshot.info->symlink) {
      snapshot.info->reason = "Symbolic links do not support mode changes. Ownership applies to the links themselves.";
    }
    const auto unavailable = std::ranges::count_if(rows, [](const auto& row) {
      return !row.info || (!row.info->canMode && !row.info->canOwner && !row.info->canGroup);
    });
    if (snapshot.info && unavailable > 0) {
      snapshot.info->reason += std::format(" {} items cannot be edited; see Results for details.", unavailable);
    }
  }
  std::string report(const std::vector<files::PermissionResult>& rows) {
    constexpr std::array labels{"Ready", "Changed", "Unchanged", "Failed", "Partially changed", "Cancelled", "Skipped"};
    std::string text = "Permission results\n";
    for (const auto& row : rows) {
      if (text.size() > 16 * 1024 * 1024) {
        text += "\nReport truncated. All results remain available in Properties.\n";
        break;
      }
      text += row.location.uri() + "\n  " + labels[static_cast<std::size_t>(row.outcome)];
      if (row.info) {
        text += std::format(" · mode {:04o} · owner {} · group {}", row.info->mode, row.info->owner, row.info->group);
      }
      if (!row.message.empty()) {
        text += "\n  " + row.message;
      }
      text += '\n';
    }
    return text;
  }
  Reply applyBulk(
      const std::shared_ptr<Bulk>& bulk, files::PermissionEdit edit, const std::atomic<bool>& stop,
      std::atomic<std::size_t>& completed, const std::optional<files::RecursivePermissionEdit>& recursive = std::nullopt
  ) {
    using O = files::PermissionOutcome;
    auto rows = std::make_shared<std::vector<files::PermissionResult>>(*bulk->results);
    for (std::size_t i = 0; i < rows->size(); ++i) {
      if (bulk->targets[i] && (*rows)[i].outcome != O::Skipped) {
        (*rows)[i].outcome = O::Ready;
        (*rows)[i].message = (*rows)[i].info ? (*rows)[i].info->reason : "";
      }
    }
    Reply reply{.target = {}, .info = {}, .message = {}, .bulk = bulk, .results = rows, .report = {}};
    try {
      // Validate shared input before any item changes, and resolve accounts just once.
      if ((edit.mask & ~07777U) != 0 || (edit.mode && (*edit.mode & ~07777U) != 0)) {
        throw std::runtime_error("Permission bits are out of range.");
      }
      if (edit.owner) {
        edit.owner = std::to_string(accountId(*edit.owner, true));
      }
      if (edit.group) {
        const auto group = accountId(*edit.group, false);
        if (::geteuid() != 0 && !member(group) && std::ranges::any_of(bulk->targets, [group](const auto& target) {
              return target && target->baseline.st_gid != group;
            })) {
          throw std::runtime_error("You must be a member of the requested group.");
        }
        edit.group = std::to_string(group);
      }
    } catch (const std::exception& error) {
      reply.error = true;
      reply.message = error.what();
      for (auto& row : *rows) {
        if (row.outcome == O::Ready) {
          row.message = "Not attempted: " + reply.message;
        }
      }
      reply.report = "No changes attempted: " + reply.message + "\n" + report(*rows);
      return reply;
    }
    struct OwnChange {
      struct stat value;
      bool changed;
    };
    const auto complete = [&edit, &recursive](const std::optional<files::PermissionInfo>& info) {
      const auto mode = recursive && info && info->directory ? recursive->directoryMode : edit.mode;
      const auto mask = recursive && info && info->directory ? recursive->directoryMask : edit.mask;
      return info
          && (!edit.owner || std::to_string(info->owner) == *edit.owner)
          && (!edit.group || std::to_string(info->group) == *edit.group)
          && (!mode || ((info->mode ^ *mode) & mask) == 0);
    };
    std::map<std::pair<dev_t, ino_t>, OwnChange> aliases;
    for (std::size_t i = 0; i < rows->size(); ++i) {
      auto& row = (*rows)[i];
      const auto& target = bulk->targets[i];
      if (row.outcome == O::Skipped) {
        completed.fetch_add(1);
        continue;
      }
      if (stop.load()) {
        if (target) {
          row.outcome = O::Cancelled;
          row.message = "Not attempted; cancelled.";
        }
        continue;
      }
      if (!target) {
        if (row.outcome != O::Skipped) {
          row.outcome = O::Failed;
        }
        completed.fetch_add(1);
        continue;
      }
      try {
        const auto key = std::pair{target->baseline.st_dev, target->baseline.st_ino};
        bool changedViaAlias = false;
        if (const auto found = aliases.find(key); found != aliases.end()
            && target->current()
            && (found->second.changed || samePermissions(target->baseline, found->second.value))
            && samePermissions(metadata(target->fd.value), found->second.value)) {
          changedViaAlias = !samePermissions(target->baseline, found->second.value);
          target->baseline = found->second.value;
        }
        auto itemEdit = edit;
        if (recursive && S_ISDIR(target->baseline.st_mode)) {
          itemEdit.mode = recursive->directoryMode;
          itemEdit.mask = recursive->directoryMask;
        }
        auto item = ::apply(target, itemEdit, stop, !recursive.has_value());
        row.info = std::move(item.info);
        row.message = std::move(item.message);
        row.outcome = item.error ? (item.applied || changedViaAlias ? O::Partial : O::Failed)
            : (item.applied || changedViaAlias) && !complete(row.info) ? O::Partial
            : item.applied || changedViaAlias                          ? O::Changed
            : stop.load()                                              ? O::Cancelled
                                                                       : O::Unchanged;
        if (row.outcome == O::Partial && !item.error) {
          row.message = "Cancelled after applying some requested changes.";
        }
        if (!item.error && changedViaAlias && !item.applied) {
          row.message = "Updated through another selected hard link.";
        }
        reply.applied = reply.applied || item.applied;
        if (!item.error || item.applied) {
          const auto previous = aliases.find(key);
          const bool changed = item.applied || (previous != aliases.end() && previous->second.changed);
          aliases.insert_or_assign(key, OwnChange{metadata(target->fd.value), changed});
        }
      } catch (const std::exception& error) {
        row.outcome = O::Failed;
        row.message = error.what();
        row.info.reset();
      }
      completed.fetch_add(1);
    }
    // Cancellation can leave unvisited names referring to an inode changed through
    // another selected hard link. Read the held inode without further account lookups.
    for (std::size_t i = 0; i < rows->size(); ++i) {
      auto& row = (*rows)[i];
      const auto& target = bulk->targets[i];
      if (row.outcome != O::Cancelled || !target || !row.info) {
        continue;
      }
      try {
        const auto value = metadata(target->fd.value);
        const auto own = aliases.find({value.st_dev, value.st_ino});
        if (own != aliases.end()
            && own->second.changed
            && samePermissions(value, own->second.value)
            && !samePermissions(value, target->baseline)) {
          row.outcome = O::Changed;
          row.message = "Changed through another selected hard link before cancellation.";
          completed.fetch_add(1);
        }
        auto& info = *row.info;
        if (info.owner != value.st_uid) {
          info.ownerName.clear();
        }
        if (info.group != value.st_gid) {
          info.groupName.clear();
        }
        info.owner = value.st_uid;
        info.group = value.st_gid;
        info.mode = value.st_mode & 07777;
        info.current = target->current();
        info.canMode = info.canOwner = info.canGroup = false;
        if (row.outcome == O::Changed && !complete(row.info)) {
          row.outcome = O::Partial;
          row.message = "Partially changed through another selected hard link before cancellation.";
        }
      } catch (const std::exception& error) {
        row.info.reset();
        row.message += std::string(" ") + error.what();
      }
    }
    for (std::size_t i = 0; i < rows->size(); ++i) {
      auto& row = (*rows)[i];
      const auto& target = bulk->targets[i];
      if (row.outcome == O::Skipped && target) {
        try {
          row.info = target->inspect(false);
          if (!samePermissions(metadata(target->fd.value), target->baseline)) {
            row.message += " Permissions changed through another name.";
          }
        } catch (const std::exception& error) {
          row.info.reset();
          row.message += std::string(" ") + error.what();
        }
      }
    }
    bulk->results = rows;
    reply.finished = true;
    constexpr std::array labels{"ready", "changed", "unchanged", "failed", "partially changed", "cancelled", "skipped"};
    for (std::size_t i = 1; i < labels.size(); ++i) {
      const auto count = std::ranges::count(*rows, static_cast<O>(i), &files::PermissionResult::outcome);
      if (count != 0) {
        if (!reply.message.empty()) {
          reply.message += " · ";
        }
        reply.message += std::format("{} {}", count, labels[i]);
      }
    }
    reply.error = std::ranges::any_of(*rows, [](const auto& row) {
      return row.outcome == O::Failed || row.outcome == O::Partial;
    });
    reply.report = report(*rows);
    return reply;
  }
  // Collection is complete before mutations begin. The item and depth limits also
  // bound retained descriptors, result memory and the traversal stack.
  class CollectionLimit final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
  };
  struct CloseDirectory {
    void operator()(DIR* directory) const { ::closedir(directory); }
  };
  std::uint64_t mountId(int fd) {
    struct statx value{};
    if (::statx(fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &value) != 0
        || (value.stx_mask & STATX_MNT_ID) == 0) {
      throw std::runtime_error("Cannot identify the filesystem mount boundary.");
    }
    return value.stx_mnt_id;
  }
  Reply recursiveJob(
      const std::shared_ptr<Bulk>& roots, files::RecursivePermissionEdit edit, const std::atomic<bool>& stop,
      std::atomic<std::size_t>& completed, std::atomic<std::size_t>& total, std::atomic<bool>& collecting
  ) {
    using O = files::PermissionOutcome;
    auto plan = std::make_shared<Bulk>();
    plan->results = std::make_shared<std::vector<files::PermissionResult>>();
    auto initial = std::make_shared<std::vector<files::PermissionResult>>(*roots->results);
    for (std::size_t i = 0; i < initial->size(); ++i) {
      if (roots->targets[i]) {
        (*initial)[i].outcome = O::Ready;
        (*initial)[i].message.clear();
      }
    }
    Reply reply{.target = {}, .info = {}, .message = {}, .bulk = roots, .results = initial, .report = {}};
    try {
      if ((edit.files.mask & ~07777U) != 0
          || (edit.files.mode && (*edit.files.mode & ~07777U) != 0)
          || (edit.directoryMask & ~07777U) != 0
          || (edit.directoryMode && (*edit.directoryMode & ~07777U) != 0)) {
        throw std::runtime_error("Permission bits are out of range.");
      }
      if (edit.files.owner) {
        edit.files.owner = std::to_string(accountId(*edit.files.owner, true));
      }
      if (edit.files.group) {
        const auto group = accountId(*edit.files.group, false);
        if (::geteuid() != 0 && !member(group)) {
          throw std::runtime_error("You must be a member of the requested group.");
        }
        edit.files.group = std::to_string(group);
      }
    } catch (const std::exception& error) {
      reply.error = true;
      reply.message = error.what();
      for (auto& row : *initial) {
        if (row.outcome == O::Ready) {
          row.message = "Not attempted: " + reply.message;
        }
      }
      reply.report = "No changes attempted: " + reply.message + "\n" + report(*initial);
      collecting.store(false);
      return reply;
    }
    const auto append = [&](const files::PermissionResult& row, std::shared_ptr<Target> target) {
      if (plan->results->size() >= 16384) {
        throw CollectionLimit("The recursive job exceeds 16384 items. Select smaller folders.");
      }
      plan->results->push_back(row);
      plan->targets.push_back(std::move(target));
    };
    std::map<std::string, std::size_t> selected;
    for (std::size_t i = 0; i < roots->results->size(); ++i) {
      selected.emplace((*roots->results)[i].location.uri(), i);
    }
    std::set<std::string> seen;
    std::set<std::pair<dev_t, ino_t>> directories;
    std::function<void(std::shared_ptr<Target>, std::uint64_t, std::size_t)> walk;
    walk = [&](std::shared_ptr<Target> target, std::uint64_t rootMount, std::size_t depth) {
      if (stop.load() || !seen.insert(target->entry.location.uri()).second) {
        return;
      }
      if (completed.fetch_add(1) >= 16384) {
        throw CollectionLimit("The recursive job exceeds 16384 items. Select smaller folders.");
      }
      files::PermissionResult row{target->entry.location, target->entry.displayName, {}, O::Ready, {}};
      try {
        target->validate();
        row.info = target->inspect(false);
        if (S_ISLNK(target->baseline.st_mode)) {
          row.outcome = O::Skipped;
          row.message = "Symbolic link skipped; its target was not followed.";
          append(row, std::move(target));
          return;
        }
        if (mountId(target->fd.value) != rootMount) {
          row.outcome = O::Skipped;
          row.message = "Mounted subtree skipped.";
          append(row, std::move(target));
          return;
        }
        if (S_ISDIR(target->baseline.st_mode)) {
          if (depth >= 128) {
            throw CollectionLimit("The recursive job exceeds 128 folder levels. Select smaller folders.");
          }
          if (!directories.emplace(target->baseline.st_dev, target->baseline.st_ino).second) {
            row.outcome = O::Skipped;
            row.message = "Folder already visited; subtree skipped.";
            append(row, std::move(target));
            return;
          }
          const int descriptor = ::openat(target->fd.value, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
          if (descriptor < 0) {
            if (errno == EMFILE || errno == ENFILE) {
              throw CollectionLimit("Not enough file descriptors to collect this job. Select smaller folders.");
            }
            throw std::runtime_error("Contents not inspected: " + std::string(std::strerror(errno)));
          }
          // fdopendir owns the descriptor only on success.
          const std::unique_ptr<DIR, CloseDirectory> directory(::fdopendir(descriptor));
          if (!directory) {
            const auto message = std::string(std::strerror(errno));
            ::close(descriptor);
            throw std::runtime_error(message);
          }
          while (!stop.load()) {
            errno = 0;
            const auto* child = ::readdir(directory.get());
            if (!child) {
              if (errno != 0) {
                throw std::runtime_error("Contents only partly inspected: " + std::string(std::strerror(errno)));
              }
              break;
            }
            const std::string name(child->d_name);
            if (name == "." || name == "..") {
              continue;
            }
            target->validate();
            auto entry = target->entry;
            entry.location = files::Location::fromPath(target->path + "/" + name);
            entry.name = name;
            const files::GString display(g_filename_display_name(name.c_str()));
            entry.displayName = display.get();
            if (const auto found = selected.find(entry.location.uri()); found != selected.end()) {
              const auto& captured = roots->targets[found->second];
              if (captured) {
                captured->ancestors = target->ancestors;
                captured->ancestors.push_back(target);
                walk(captured, mountId(captured->fd.value), depth + 1);
              } else if (seen.insert(entry.location.uri()).second) {
                append((*roots->results)[found->second], {});
              }
              continue;
            }
            if (seen.contains(entry.location.uri())) {
              continue;
            }
            const int fd = ::openat(::dirfd(directory.get()), name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) {
              if (errno == EMFILE || errno == ENFILE) {
                throw CollectionLimit("Not enough file descriptors to collect this job. Select smaller folders.");
              }
              append({entry.location, entry.displayName, {}, O::Failed, std::strerror(errno)}, {});
              continue;
            }
            auto ancestors = target->ancestors;
            ancestors.push_back(target);
            walk(std::make_shared<Target>(std::move(entry), fd, std::move(ancestors)), rootMount, depth + 1);
          }
        }
        // Postorder keeps parent access intact while processing descendants.
        append(row, std::move(target));
      } catch (const CollectionLimit&) {
        throw;
      } catch (const std::exception& error) {
        row.outcome = O::Failed;
        row.message = error.what();
        append(row, {});
      }
    };
    try {
      std::vector<std::size_t> order;
      order.reserve(roots->targets.size());
      for (std::size_t i = 0; i < roots->targets.size(); ++i) {
        order.push_back(i);
      }
      std::ranges::stable_sort(order, {}, [&](std::size_t index) {
        return (*roots->results)[index].location.uri().size();
      });
      for (const auto i : order) {
        if (stop.load()) {
          break;
        }
        const auto& target = roots->targets[i];
        if (target) {
          // An explicitly selected descendant is reused by its ancestor's walk.
          // Sorting roots by path length ensures ancestors are visited first.
          walk(target, mountId(target->fd.value), 0);
        } else if (seen.insert((*roots->results)[i].location.uri()).second) {
          append((*roots->results)[i], {});
        }
      }
      if (!stop.load()) {
        total.store(plan->results->size());
        completed.store(0);
        collecting.store(false);
        reply = applyBulk(plan, edit.files, stop, completed, edit);
        reply.bulk = roots; // Release collected descriptors when the worker returns.
        reply.finished = true;
        return reply;
      }
      reply.message = "Cancelled while collecting targets. No changes attempted; total unknown.";
    } catch (const std::exception& error) {
      reply.error = true;
      reply.message = "No changes attempted: " + std::string(error.what());
    }
    collecting.store(false);
    reply.finished = true;
    reply.results = plan->results;
    for (auto& row : *reply.results) {
      if (row.outcome == O::Ready) {
        row.outcome = O::Cancelled;
        row.message = "Not attempted; collection did not complete.";
      }
    }
    reply.report = reply.message + "\n" + report(*reply.results);
    return reply;
  }
} // namespace

struct files::PermissionService::State {
  PermissionSnapshot snapshot;
  std::shared_ptr<Target> target;
  std::shared_ptr<Bulk> bulk;
  std::atomic<std::size_t> completed{0};
  std::atomic<std::size_t> total{0};
  std::atomic<bool> collecting{false};
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
    owner->snapshot.cancelRequested = false;
    const GioPtr<GTask> task(g_task_new(
        nullptr, nullptr,
        [](GObject*, GAsyncResult* result, gpointer) {
          const auto& request = *static_cast<Request*>(g_task_get_task_data(G_TASK(result)));
          const auto owner = request.owner;
          const std::unique_ptr<Reply> reply(static_cast<Reply*>(g_task_propagate_pointer(G_TASK(result), nullptr)));
          owner->active = false;
          owner->collecting.store(false);
          owner->snapshot.info = reply->info;
          owner->snapshot.message = reply->message;
          owner->snapshot.error = reply->error;
          owner->snapshot.applied = reply->applied;
          owner->target = reply->target;
          owner->bulk = reply->bulk;
          owner->snapshot.results = reply->results;
          owner->snapshot.finished = reply->finished;
          owner->snapshot.report = std::move(reply->report);
          if (reply->bulk) {
            summarize(owner->snapshot, *reply->results);
          }
          owner->snapshot.phase = owner->stop.load() && (!request.saving || reply->bulk) ? PermissionPhase::Cancelled
              : (reply->info || reply->bulk)                                             ? PermissionPhase::Ready
                                                                                         : PermissionPhase::Error;
          if (owner->stop.load()) {
            owner->target.reset();
            owner->bulk.reset();
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
std::size_t files::PermissionService::completed() const { return m_state->completed.load(); }
bool files::PermissionService::collecting() const { return m_state->collecting.load(); }
std::size_t files::PermissionService::total() const { return m_state->total.load(); }
const files::PermissionSnapshot& files::PermissionService::snapshot() const { return m_state->snapshot; }
void files::PermissionService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
void files::PermissionService::cancel() {
  m_state->stop.store(true);
  if (!busy()) {
    m_state->target.reset();
    m_state->bulk.reset();
  } else {
    m_state->snapshot.cancelRequested = true;
    m_state->notify();
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
bool files::PermissionService::prepare(std::vector<DirectoryEntry> entries) {
  if (busy() || entries.size() < 2 || entries.size() > 4096) {
    return false;
  }
  m_state->target.reset();
  m_state->bulk.reset();
  m_state->completed.store(0);
  m_state->snapshot = {};
  m_state->snapshot.phase = PermissionPhase::Loading;
  m_state->snapshot.multiple = true;
  State::launch(
      m_state,
      [owner = m_state, entries = std::move(entries)] {
        auto bulk = std::make_shared<Bulk>();
        bulk->results = std::make_shared<std::vector<PermissionResult>>();
        std::set<std::string> uris;
        for (const auto& entry : entries) {
          if (!uris.insert(entry.location.uri()).second) {
            throw std::runtime_error("The selection contains repeated locations.");
          }
          bulk->results->push_back(
              {entry.location, entry.displayName, {}, PermissionOutcome::Cancelled, "Not inspected; cancelled."}
          );
          bulk->targets.push_back({});
        }
        for (std::size_t i = 0; i < entries.size() && !owner->stop.load(); ++i) {
          auto& row = (*bulk->results)[i];
          try {
            auto target = prepareTarget(entries[i], false);
            row.info = target->inspect();
            row.outcome = PermissionOutcome::Ready;
            row.message = row.info->reason;
            bulk->targets[i] = std::move(target);
          } catch (const std::exception& error) {
            row.outcome = PermissionOutcome::Failed;
            row.message = error.what();
          }
          owner->completed.fetch_add(1);
        }
        return Reply{
            .target = {},
            .info = {},
            .message = {},
            .bulk = bulk,
            .results = bulk->results,
            .report = report(*bulk->results)
        };
      },
      false
  );
  return true;
}
bool files::PermissionService::prepareEntry(DirectoryEntry entry, bool captureCurrentLocation) {
  if (busy()) {
    return false;
  }
  m_state->target.reset();
  m_state->bulk.reset();
  m_state->completed.store(0);
  m_state->snapshot = {};
  m_state->snapshot.phase = PermissionPhase::Loading;
  State::launch(
      m_state,
      [owner = m_state, entry = std::move(entry), captureCurrentLocation] {
        auto target = prepareTarget(entry, captureCurrentLocation);
        if (owner->stop.load()) {
          return Reply{};
        }
        return Reply{
            .target = target, .info = target->inspect(), .message = {}, .bulk = {}, .results = {}, .report = {}
        };
      },
      false
  );
  return true;
}
bool files::PermissionService::apply(PermissionEdit edit) {
  if (busy()
      || (!m_state->target && !m_state->bulk)
      || m_state->snapshot.phase != PermissionPhase::Ready
      || (m_state->snapshot.recursive && m_state->snapshot.finished)) {
    return false;
  }
  const auto& info = m_state->snapshot.info;
  if (!info || (!info->canMode && !info->canOwner && !info->canGroup)) {
    return false;
  }
  m_state->snapshot.recursive = false;
  if (m_state->bulk) {
    m_state->snapshot.phase = PermissionPhase::Saving;
    m_state->snapshot.finished = false;
    m_state->snapshot.error = m_state->snapshot.applied = false;
    m_state->snapshot.message.clear();
    m_state->completed.store(0);
    State::launch(
        m_state,
        [owner = m_state, bulk = m_state->bulk, edit = std::move(edit)] {
          return applyBulk(bulk, edit, owner->stop, owner->completed);
        },
        true
    );
    return true;
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
bool files::PermissionService::applyRecursive(RecursivePermissionEdit edit) {
  if (busy()
      || m_state->snapshot.phase != PermissionPhase::Ready
      || (m_state->snapshot.recursive && m_state->snapshot.finished)
      || (!m_state->target && !m_state->bulk)) {
    return false;
  }
  auto roots = m_state->bulk;
  if (!roots) {
    roots = std::make_shared<Bulk>();
    roots->targets.push_back(m_state->target);
    roots->results = std::make_shared<std::vector<PermissionResult>>();
    roots->results->push_back(
        {m_state->target->entry.location,
         m_state->target->entry.displayName,
         m_state->snapshot.info,
         PermissionOutcome::Ready,
         {}}
    );
  }
  m_state->snapshot.phase = PermissionPhase::Saving;
  m_state->snapshot.recursive = m_state->snapshot.multiple = true;
  m_state->snapshot.finished = m_state->snapshot.error = m_state->snapshot.applied = false;
  m_state->snapshot.message.clear();
  m_state->completed.store(0);
  m_state->total.store(0);
  m_state->collecting.store(true);
  State::launch(
      m_state,
      [owner = m_state, roots, edit = std::move(edit)] {
        return recursiveJob(roots, edit, owner->stop, owner->completed, owner->total, owner->collecting);
      },
      true
  );
  return true;
}
