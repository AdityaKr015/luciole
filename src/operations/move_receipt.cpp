#include "operations/move_receipt.h"

#include "fs/background_job.h"
#include "fs/file_error.h"
#include "fs/posix_fd.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <linux/fs.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <utility>

struct luciole::MoveIdentity {
  Fd from, to, item;
};

namespace {
  luciole::Fd currentParent(const luciole::Location& location, int held) {
    auto parent = location.parent();
    if (!parent || !parent->isNative()) {
      luciole::throwFileError(
          luciole::FileErrorKind::InvalidArgument, "Move history requires native containing folders."
      );
    }
    auto current = luciole::checkedFd(::open(parent->nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!luciole::sameIdentity(luciole::statFd(current.get()), luciole::statFd(held))) {
      luciole::throwFileError(
          luciole::FileErrorKind::Changed, "A containing folder changed. Move history is no longer valid."
      );
    }
    return current;
  }
  struct stat namedStamp(int parent, const luciole::Location& location) {
    struct stat value{};
    const auto name = location.basename();
    if (!name || name->empty() || *name == "." || *name == "..") {
      luciole::throwFileError(luciole::FileErrorKind::InvalidArgument, "Invalid move history name.");
    }
    if (::fstatat(parent, name->c_str(), &value, AT_SYMLINK_NOFOLLOW) != 0) {
      luciole::throwErrno(errno);
    }
    return value;
  }
  bool invalidates(const luciole::FileError& error) {
    return error.kind == luciole::FileErrorKind::Changed
        || error.kind == luciole::FileErrorKind::Missing
        || error.kind == luciole::FileErrorKind::NotDirectory;
  }
  void checkCancelled(const luciole::CancelToken& token) {
    if (token.cancelled()) {
      luciole::throwFileError(luciole::FileErrorKind::Cancelled, "Move reversal cancelled.");
    }
  }
} // namespace

luciole::MoveEffect::MoveEffect(
    Location source, Location destination, std::shared_ptr<const MoveIdentity> identity, struct stat revision,
    bool reversed
)
    : m_source(std::move(source)), m_destination(std::move(destination)), m_identity(std::move(identity)),
      m_revision(revision), m_reversed(reversed) {}

int luciole::MoveEffect::sourceParent() const { return m_reversed ? m_identity->to.get() : m_identity->from.get(); }
int luciole::MoveEffect::destinationParent() const {
  return m_reversed ? m_identity->from.get() : m_identity->to.get();
}

void luciole::MoveEffect::validate(const struct stat& revision) const {
  (void)currentParent(m_source, sourceParent());
  const auto destination = currentParent(m_destination, destinationParent());
  if (!sameFileRevision(revision, statFd(m_identity->item.get()))
      || !sameFileRevision(revision, namedStamp(destination.get(), m_destination))) {
    throwFileError(
        FileErrorKind::Changed,
        "Moved item changed: " + m_destination.displayName().value_or("Item") + ". Move history is no longer valid."
    );
  }
}

void luciole::MoveEffect::requireVacant() const {
  const auto target = currentParent(m_source, sourceParent());
  struct stat value{};
  if (::fstatat(target.get(), m_source.basename()->c_str(), &value, AT_SYMLINK_NOFOLLOW) == 0) {
    throwFileError(
        FileErrorKind::Exists, "The return name is occupied: " + m_source.displayName().value_or("Item") + "."
    );
  }
  if (errno != ENOENT) {
    throwErrno(errno);
  }
}

std::shared_ptr<const luciole::MoveEffect>
luciole::MoveEffect::capture(const Location& source, const Location& destination, int from, int to, int item) {
  if (source == destination || source.isTrash() || destination.isTrash()) {
    throwFileError(FileErrorKind::InvalidArgument, "Invalid native move history.");
  }
  auto identity = std::make_unique<MoveIdentity>(
      checkedFd(::fcntl(from, F_DUPFD_CLOEXEC, 0)), checkedFd(::fcntl(to, F_DUPFD_CLOEXEC, 0)),
      checkedFd(::fcntl(item, F_DUPFD_CLOEXEC, 0))
  );
  std::shared_ptr<const MoveIdentity> proof(
      identity.release(), [worker = std::this_thread::get_id()](const MoveIdentity* retired) {
        auto owned = std::unique_ptr<const MoveIdentity>(retired);
        if (std::this_thread::get_id() == worker) {
          // Optional capture must release descriptor pressure immediately on its worker.
          return;
        }
        detail::runInThread([owned = std::move(owned)] mutable { owned.reset(); }, [] {});
      }
  );
  auto effect = std::shared_ptr<MoveEffect>(new MoveEffect(source, destination, std::move(proof), statFd(item), false));
  effect->validate(effect->m_revision);
  return effect;
}

std::shared_ptr<const luciole::MoveReceipt> luciole::MoveReceipt::capture(
    const std::vector<std::shared_ptr<const MoveEffect>>& effects, const Revisions& revisions
) {
  if (effects.empty() || effects.size() > 4096) {
    return {};
  }
  auto receipt = std::make_shared<MoveReceipt>();
  for (const auto& effect : effects) {
    auto expected = effect->m_revision;
    if (const auto found = revisions.find({expected.st_dev, expected.st_ino}); found != revisions.end()) {
      // Renaming another hard-link alias changes ctime for every alias of this inode.
      expected.st_ctim = found->second.st_ctim;
      if (!sameFileRevision(expected, found->second)) {
        throwFileError(FileErrorKind::Changed, "An item changed while move history was captured.");
      }
    }
    effect->validate(expected);
    receipt->m_effects.push_back(
        std::shared_ptr<const MoveEffect>(
            new MoveEffect(effect->m_source, effect->m_destination, effect->m_identity, expected, effect->m_reversed)
        )
    );
  }
  return receipt;
}

luciole::MoveReplayResult
luciole::MoveReceipt::reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const {
  MoveReplayResult reply;
  reply.results.reserve(m_effects.size());
  for (const auto& effect : m_effects) {
    reply.results.emplace_back(effect->m_destination, TransferOutcome::Skipped);
  }
  try {
    for (const auto& effect : m_effects) {
      checkCancelled(token);
      effect->validate(effect->m_revision);
      effect->requireVacant();
    }
  } catch (const std::exception&) {
    const auto error = currentFileError();
    reply.retireHistory = invalidates(error);
    for (auto& result : reply.results) {
      result.outcome = error.kind == FileErrorKind::Cancelled ? TransferOutcome::Cancelled : TransferOutcome::Failed;
      result.error = error;
    }
    completed.store(reply.results.size());
    return reply;
  }
  Revisions revisions;
  std::vector<std::shared_ptr<const MoveEffect>> inverse;
  bool stopped = false, applied = false;
  for (std::size_t i = 0; i < m_effects.size(); ++i) {
    auto& result = reply.results[i];
    const auto& effect = m_effects[i];
    if (stopped) {
      result.outcome = token.cancelled() ? TransferOutcome::Cancelled : TransferOutcome::Skipped;
      result.error = FileError{
          .kind = token.cancelled() ? FileErrorKind::Cancelled : FileErrorKind::Other,
          .message = "Not attempted after move reversal stopped."
      };
      completed.fetch_add(1);
      continue;
    }
    try {
      checkCancelled(token);
      auto expected = effect->m_revision;
      if (const auto found = revisions.find({expected.st_dev, expected.st_ino}); found != revisions.end()) {
        expected = found->second;
      }
      effect->validate(expected);
      effect->requireVacant();
      const auto from = currentParent(effect->m_destination, effect->destinationParent());
      const auto to = currentParent(effect->m_source, effect->sourceParent());
      checkCancelled(token);
      if (::syscall(
              SYS_renameat2, from.get(), effect->m_destination.basename()->c_str(), to.get(),
              effect->m_source.basename()->c_str(), RENAME_NOREPLACE
          )
          != 0) {
        throwErrno(errno);
      }
      applied = true;
      result.outcome = TransferOutcome::Moved;
      result.destination = effect->m_source;
      const auto current = statFd(effect->m_identity->item.get());
      expected.st_ctim = current.st_ctim;
      if (!sameFileRevision(expected, current) || !sameFileRevision(current, namedStamp(to.get(), effect->m_source))) {
        throwFileError(
            FileErrorKind::Changed, "The item changed during move reversal. Inspect the reported locations."
        );
      }
      revisions[{current.st_dev, current.st_ino}] = current;
      inverse.push_back(
          std::shared_ptr<const MoveEffect>(
              new MoveEffect(effect->m_destination, effect->m_source, effect->m_identity, current, !effect->m_reversed)
          )
      );
    } catch (const std::exception&) {
      const auto error = currentFileError();
      reply.retireHistory |= invalidates(error);
      if (result.outcome == TransferOutcome::Moved) {
        result.outcome = TransferOutcome::Partial;
      } else {
        result.outcome = error.kind == FileErrorKind::Cancelled ? TransferOutcome::Cancelled : TransferOutcome::Failed;
      }
      result.error = error;
      stopped = true;
    }
    completed.fetch_add(1);
  }
  if (!stopped) {
    try {
      reply.receipt = capture(inverse, revisions);
    } catch (const FileErrorException&) {
      // All moves committed; optional inverse capture cannot turn them into failed operations.
    }
  }
  reply.retireHistory |= applied && !reply.receipt;
  return reply;
}

std::size_t luciole::transferHistoryBudget(std::size_t descriptorsPerRoot) {
  struct rlimit limit{};
  if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    return 0;
  }
  std::error_code error;
  std::size_t used = 0;
  std::filesystem::directory_iterator it("/proc/self/fd", error), end;
  while (!error && it != end) {
    ++used;
    it.increment(error);
  }
  if (error || !descriptorsPerRoot || limit.rlim_cur <= used + 128) {
    return 0;
  }
  return static_cast<std::size_t>(std::min<rlim_t>(4096, (limit.rlim_cur - used - 128) / descriptorsPerRoot));
}

std::vector<luciole::TransferResult> luciole::MoveReceipt::unconfirmed(const FileError& error) const {
  std::vector<TransferResult> results;
  for (const auto& effect : m_effects) {
    auto& result = results.emplace_back(effect->m_destination, TransferOutcome::Partial, error);
    result.destination = effect->m_source;
    result.error->message =
        "Move reversal could not be confirmed. Inspect both reported locations. " + result.error->message;
  }
  return results;
}
