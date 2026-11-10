#include "operations/rename_receipt.h"

#include "fs/file_error.h"
#include "fs/posix_fd.h"

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

struct files::RenameIdentity {
  Fd parent, item;
  struct stat stamp;
};

namespace {
  bool sameRevision(const struct stat& a, const struct stat& b) {
    return files::sameIdentity(a, b)
        && a.st_size == b.st_size
        && a.st_mode == b.st_mode
        && a.st_uid == b.st_uid
        && a.st_gid == b.st_gid
        && a.st_nlink == b.st_nlink
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec
        && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec
        && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
  }
  std::pair<std::string, std::string> names(const files::Location& source, const files::Location& destination) {
    if (!source.isNative()
        || !destination.isNative()
        || source.parent() != destination.parent()
        || source == destination
        || source.isTrash()
        || destination.isTrash()) {
      files::throwFileError(files::FileErrorKind::InvalidArgument, "Invalid rename history entry.");
    }
    const auto before = std::filesystem::path(*source.nativePath()).filename().native();
    const auto after = std::filesystem::path(*destination.nativePath()).filename().native();
    if (before.empty() || before == "." || before == ".." || after.empty() || after == "." || after == "..") {
      files::throwFileError(files::FileErrorKind::InvalidArgument, "Invalid rename history name.");
    }
    return {before, after};
  }
  struct stat namedStamp(int parent, const std::string& name) {
    struct stat stamp{};
    if (::fstatat(parent, name.c_str(), &stamp, AT_SYMLINK_NOFOLLOW) < 0) {
      files::throwErrno(errno);
    }
    return stamp;
  }
} // namespace

files::RenameReceipt::RenameReceipt(
    Location source, Location destination, std::shared_ptr<const RenameIdentity> identity
)
    : m_source(std::move(source)), m_destination(std::move(destination)), m_identity(std::move(identity)) {}

std::shared_ptr<const files::RenameReceipt>
files::RenameReceipt::capture(const Location& source, const Location& destination, int parent, int item) {
  const auto [before, after] = names(source, destination);
  const auto stamp = statFd(item);
  if (!sameRevision(stamp, namedStamp(parent, after))) {
    throwFileError(FileErrorKind::Changed, "The renamed item changed before history was captured.");
  }
  const auto currentParent =
      checkedFd(::open(source.parent()->nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!sameIdentity(statFd(parent), statFd(currentParent.get()))) {
    throwFileError(FileErrorKind::Changed, "The containing folder changed before history was captured.");
  }
  auto identity = std::make_shared<RenameIdentity>(
      checkedFd(::fcntl(parent, F_DUPFD_CLOEXEC, 0)), checkedFd(::fcntl(item, F_DUPFD_CLOEXEC, 0)), stamp
  );
  return std::make_shared<RenameReceipt>(source, destination, std::move(identity));
}

std::shared_ptr<const files::RenameReceipt> files::RenameReceipt::reverse(const CancelToken& token) const {
  if (!m_identity) {
    throwFileError(FileErrorKind::InvalidArgument, "Invalid rename history entry.");
  }
  const auto [before, after] = names(m_source, m_destination);
  const auto parent =
      checkedFd(::open(m_destination.parent()->nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!sameIdentity(statFd(parent.get()), statFd(m_identity->parent.get()))) {
    throwFileError(FileErrorKind::Changed, "The containing folder changed. Rename history is no longer valid.");
  }
  if (!sameRevision(m_identity->stamp, statFd(m_identity->item.get()))
      || !sameRevision(m_identity->stamp, namedStamp(parent.get(), after))) {
    throwFileError(FileErrorKind::Changed, "The renamed item changed. Rename history is no longer valid.");
  }
  if (token.cancelled()) {
    throwFileError(FileErrorKind::Cancelled, "Rename reversal cancelled.");
  }
  if (::syscall(SYS_renameat2, parent.get(), after.c_str(), parent.get(), before.c_str(), RENAME_NOREPLACE) != 0) {
    throwErrno(errno);
  }
  try {
    return capture(m_destination, m_source, parent.get(), m_identity->item.get());
  } catch (const FileErrorException&) {
    // The rename already committed. Failure to capture history must not report it as an unapplied operation.
    return {};
  }
}
