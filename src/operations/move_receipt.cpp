#include "operations/move_receipt.h"

#include "fs/background_job.h"
#include "fs/file_error.h"
#include "fs/posix_fd.h"
#include "operations/copy_receipt.h"
#include "operations/transfer_copy.h"

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
  std::shared_ptr<const luciole::MoveIdentity> ownIdentity(luciole::Fd from, luciole::Fd to, luciole::Fd item) {
    auto identity = std::make_unique<luciole::MoveIdentity>(std::move(from), std::move(to), std::move(item));
    return std::shared_ptr<const luciole::MoveIdentity>(
        identity.release(), [worker = std::this_thread::get_id()](const luciole::MoveIdentity* retired) {
          auto owned = std::unique_ptr<const luciole::MoveIdentity>(retired);
          if (std::this_thread::get_id() == worker) {
            return;
          }
          luciole::detail::runInThread([owned = std::move(owned)] mutable { owned.reset(); }, [] {});
        }
    );
  }
  luciole::Fd currentParent(const luciole::Location& location, int held, const luciole::MoveParents& parents = {}) {
    auto parent = location.parent();
    if (!parent || !parent->isNative()) {
      luciole::throwFileError(
          luciole::FileErrorKind::InvalidArgument, "Move history requires native containing folders."
      );
    }
    if (const auto planned = parents.find(parent->uri()); planned != parents.end()) {
      if (!luciole::sameIdentity(luciole::statFd(planned->second), luciole::statFd(held))) {
        luciole::throwFileError(luciole::FileErrorKind::Changed, "The retained source parent changed.");
      }
      return luciole::checkedFd(::fcntl(held, F_DUPFD_CLOEXEC, 0));
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
    bool reversed, std::shared_ptr<const CopyEffect> previous
)
    : m_source(std::move(source)), m_destination(std::move(destination)), m_identity(std::move(identity)),
      m_revision(revision), m_reversed(reversed), m_previous(std::move(previous)) {}

int luciole::MoveEffect::sourceParent() const { return m_reversed ? m_identity->to.get() : m_identity->from.get(); }
int luciole::MoveEffect::destinationParent() const {
  return m_reversed ? m_identity->from.get() : m_identity->to.get();
}

void luciole::MoveEffect::validate(const struct stat& revision, const MoveParents& parents) const {
  (void)currentParent(m_source, sourceParent(), parents);
  const auto destination = currentParent(m_destination, destinationParent(), parents);
  if (m_previous) {
    m_previous->validate({});
  }
  if (!sameFileRevision(revision, statFd(m_identity->item.get()))
      || !sameFileRevision(revision, namedStamp(destination.get(), m_destination))) {
    throwFileError(
        FileErrorKind::Changed,
        "Moved item changed: " + m_destination.displayName().value_or("Item") + ". Move history is no longer valid."
    );
  }
}

void luciole::MoveEffect::requireVacant(const MoveParents& parents) const {
  const auto target = currentParent(m_source, sourceParent(), parents);
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
  auto proof = ownIdentity(
      checkedFd(::fcntl(from, F_DUPFD_CLOEXEC, 0)), checkedFd(::fcntl(to, F_DUPFD_CLOEXEC, 0)),
      checkedFd(::fcntl(item, F_DUPFD_CLOEXEC, 0))
  );
  auto effect = std::shared_ptr<MoveEffect>(new MoveEffect(source, destination, std::move(proof), statFd(item), false));
  effect->validate(effect->m_revision);
  return effect;
}

std::shared_ptr<const luciole::MoveEffect> luciole::MoveEffect::prepareReplacement(
    const Location& source, const Location& destination, int from, int to, std::size_t& remaining,
    const CancelToken& token
) {
  auto item = checkedFd(::openat(from, source.basename()->c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
  if (statFd(item.get()).st_dev != statFd(to).st_dev) {
    return {};
  }
  auto previous = CopyEffect::prepareAt(destination, to, to, *destination.basename(), true, remaining, token);
  auto identity = ownIdentity(
      checkedFd(::fcntl(from, F_DUPFD_CLOEXEC, 0)), checkedFd(::fcntl(to, F_DUPFD_CLOEXEC, 0)), std::move(item)
  );
  const auto revision = statFd(identity->item.get());
  return std::shared_ptr<const MoveEffect>(
      new MoveEffect(source, destination, std::move(identity), revision, false, std::move(previous))
  );
}

std::shared_ptr<const luciole::MoveEffect> luciole::MoveEffect::publishedReplacement() const {
  auto expected = m_revision;
  expected.st_ctim = statFd(m_identity->item.get()).st_ctim;
  if (!sameFileRevision(expected, statFd(m_identity->item.get()))
      || !sameFileRevision(expected, namedStamp(destinationParent(), m_destination))) {
    throwFileError(FileErrorKind::Changed, "The incoming move changed during replacement. Inspect both locations.");
  }
  auto previous = m_previous->retainedFrom(sourceParent(), *m_source.basename());
  return std::shared_ptr<const MoveEffect>(
      new MoveEffect(m_source, m_destination, m_identity, expected, false, std::move(previous))
  );
}

std::vector<luciole::Location> luciole::MoveEffect::retainForRecovery() const {
  return m_previous ? m_previous->retainForRecovery() : std::vector<Location>{};
}
std::vector<luciole::Location> luciole::MoveFolder::retainForRecovery() const { return m_item->retainForRecovery(); }

std::shared_ptr<const luciole::MoveFolder> luciole::MoveFolder::retain(
    const Location& source, int from, const Location& folder, int target, const CancelToken& token
) {
  const auto name = *source.basename();
  if (transfer::info(from, name).st_dev != statFd(target).st_dev) {
    return {};
  }
  std::size_t remaining = 1;
  auto proof = CopyEffect::prepareAt(folder.child(name), target, from, name, true, remaining, token);
  auto result = std::shared_ptr<MoveFolder>(new MoveFolder(source, folder));
  result->m_from = std::make_shared<const Fd>(checkedFd(::fcntl(from, F_DUPFD_CLOEXEC, 0)));
  result->m_target = std::make_shared<const Fd>(checkedFd(::fcntl(target, F_DUPFD_CLOEXEC, 0)));
  result->m_item = proof->retainedFrom(from, name);
  return result;
}

luciole::MoveParents luciole::MoveReceipt::parents() const {
  MoveParents result;
  for (const auto& folder : m_folders) {
    if (folder->m_item->m_hidden) {
      result.emplace(folder->m_source.uri(), folder->m_item->item());
    }
  }
  return result;
}

std::shared_ptr<const luciole::MoveReceipt> luciole::MoveReceipt::capture(
    const std::vector<std::shared_ptr<const MoveEffect>>& effects, const Revisions& revisions,
    const std::vector<std::shared_ptr<const MoveFolder>>& folders, bool undone
) {
  if (effects.size() + folders.size() == 0 || effects.size() + folders.size() > 4096) {
    return {};
  }
  auto receipt = std::make_shared<MoveReceipt>();
  receipt->m_folders = folders;
  receipt->m_undone = undone;
  const auto bindings = receipt->parents();
  for (const auto& folder : folders) {
    folder->m_item->validate({});
    (void)currentParent(folder->m_retainingFolder.child("item"), folder->m_target->get());
    (void)currentParent(folder->m_source, folder->m_from->get(), bindings);
  }
  for (const auto& effect : effects) {
    auto expected = effect->m_revision;
    if (const auto found = revisions.find({expected.st_dev, expected.st_ino}); found != revisions.end()) {
      expected.st_ctim = found->second.st_ctim;
      if (!sameFileRevision(expected, found->second)) {
        throwFileError(FileErrorKind::Changed, "An item changed while move history was captured.");
      }
    }
    effect->validate(expected, bindings);
    receipt->m_effects.push_back(
        std::shared_ptr<const MoveEffect>(new MoveEffect(
            effect->m_source, effect->m_destination, effect->m_identity, expected, effect->m_reversed,
            effect->m_previous
        ))
    );
  }
  return receipt;
}

luciole::MoveReplayResult
luciole::MoveReceipt::reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const {
  MoveReplayResult reply;
  reply.results.reserve(size());
  for (const auto& effect : m_effects) {
    reply.results.emplace_back(effect->m_destination, TransferOutcome::Skipped);
  }
  for (const auto& folder : m_folders) {
    reply.results.emplace_back(folder->m_source, TransferOutcome::Skipped);
  }
  std::unique_lock actionLock(*m_mutex, std::try_to_lock);
  std::vector<std::unique_lock<std::mutex>> locks;
  bool ownsProof = false;
  try {
    if (!actionLock.owns_lock()) {
      throwFileError(FileErrorKind::Other, "Another move reversal is using this history.");
    }
    for (const auto& effect : m_effects) {
      if (effect->m_previous) {
        locks.emplace_back(effect->m_previous->mutex(), std::try_to_lock);
        if (!locks.back().owns_lock()) {
          throwFileError(FileErrorKind::Other, "Retained move data is in use.");
        }
      }
    }
    for (const auto& folder : m_folders) {
      locks.emplace_back(folder->m_item->mutex(), std::try_to_lock);
      if (!locks.back().owns_lock()) {
        throwFileError(FileErrorKind::Other, "Retained merge data is in use.");
      }
    }
    ownsProof = std::ranges::all_of(
                    m_effects, [](const auto& effect) { return !effect->m_previous || effect->m_previous->ownsProof(); }
                )
        && std::ranges::all_of(m_folders, [](const auto& folder) { return folder->m_item->ownsProof(); });
    if (!ownsProof) {
      throwFileError(FileErrorKind::Changed, "The move history entry is stale.");
    }
    const auto bindings = parents();
    for (const auto& folder : m_folders) {
      checkCancelled(token);
      folder->m_item->validate(token);
      (void)currentParent(folder->m_retainingFolder.child("item"), folder->m_target->get());
      const auto parent = currentParent(folder->m_source, folder->m_from->get(), bindings);
      if (!m_undone) {
        struct stat occupied{};
        if (::fstatat(parent.get(), folder->m_source.basename()->c_str(), &occupied, AT_SYMLINK_NOFOLLOW) == 0) {
          throwFileError(FileErrorKind::Exists, "A merged source folder's return name is occupied.");
        }
        if (errno != ENOENT) {
          throwErrno(errno);
        }
      }
    }
    for (const auto& effect : m_effects) {
      checkCancelled(token);
      effect->validate(effect->m_revision, bindings);
      if (!effect->m_reversed || !effect->m_previous) {
        effect->requireVacant(bindings);
      }
    }
  } catch (const std::exception&) {
    const auto error = currentFileError();
    reply.retireHistory = invalidates(error);
    for (auto& result : reply.results) {
      result.outcome = error.kind == FileErrorKind::Cancelled ? TransferOutcome::Cancelled : TransferOutcome::Failed;
      result.error = error;
    }
    completed.store(size());
    if (reply.retireHistory && ownsProof) {
      retainIncomplete(reply.results);
    }
    return reply;
  }
  Revisions revisions;
  std::vector<std::shared_ptr<const MoveEffect>> inverse;
  std::vector<std::shared_ptr<const MoveFolder>> folders = m_folders;
  bool stopped = false, applied = false;
  // Forward capture orders merged containers from children to parents. Restore parents first.
  if (!m_undone) {
    for (std::size_t n = folders.size(); n > 0; --n) {
      auto& result = reply.results[m_effects.size() + n - 1];
      const auto& folder = folders[n - 1];
      bool changed = false;
      try {
        checkCancelled(token);
        auto restored = std::make_shared<MoveFolder>(*folder);
        result.source = folder->m_item->currentLocation();
        result.destination = folder->m_source;
        restored->m_item = folder->m_item->restoreTo(folder->m_source, folder->m_from->get(), changed);
        applied |= changed;
        folders[n - 1] = std::move(restored);
        result.outcome = TransferOutcome::Moved;
      } catch (const std::exception&) {
        applied |= changed;
        result.error = currentFileError();
        result.outcome = changed                             ? TransferOutcome::Partial
            : result.error->kind == FileErrorKind::Cancelled ? TransferOutcome::Cancelled
                                                             : TransferOutcome::Failed;
        stopped = true;
        break;
      }
      completed.fetch_add(1);
    }
  }
  for (std::size_t i = 0; i < m_effects.size() && !stopped; ++i) {
    auto& result = reply.results[i];
    const auto& effect = m_effects[i];
    bool changed = false;
    try {
      checkCancelled(token);
      auto expected = effect->m_revision;
      if (const auto found = revisions.find({expected.st_dev, expected.st_ino}); found != revisions.end()) {
        expected = found->second;
      }
      // Merge containers are now restored; native bindings must resolve normally.
      effect->validate(expected);
      auto previous = effect->m_previous;
      if (effect->m_reversed && previous) {
        const auto original = previous->currentLocation();
        previous = previous->storeTo(*previous->m_destination.parent(), previous->parent(), changed);
        applied |= changed;
        result.children.emplace_back(original, TransferOutcome::Moved).destination = previous->currentLocation();
      }
      effect->requireVacant();
      const auto from = currentParent(effect->m_destination, effect->destinationParent());
      const auto to = currentParent(effect->m_source, effect->sourceParent());
      // Once a paired predecessor is stored, finish this item's move before observing cancellation again.
      if (!previous || !effect->m_reversed) {
        checkCancelled(token);
      }
      if (::syscall(
              SYS_renameat2, from.get(), effect->m_destination.basename()->c_str(), to.get(),
              effect->m_source.basename()->c_str(), RENAME_NOREPLACE
          )
          != 0) {
        throwErrno(errno);
      }
      changed = applied = true;
      result.outcome = TransferOutcome::Moved;
      result.destination = effect->m_source;
      const auto current = statFd(effect->m_identity->item.get());
      expected.st_ctim = current.st_ctim;
      if (!sameFileRevision(expected, current) || !sameFileRevision(current, namedStamp(to.get(), effect->m_source))) {
        throwFileError(
            FileErrorKind::Changed, "The item changed during move reversal. Inspect the reported locations."
        );
      }
      if (!effect->m_reversed && previous) {
        const auto retained = previous->currentLocation();
        previous = previous->restoreTo(previous->m_destination, previous->parent(), changed);
        result.children.emplace_back(retained, TransferOutcome::Moved).destination = previous->currentLocation();
      }
      revisions[{current.st_dev, current.st_ino}] = current;
      inverse.push_back(
          std::shared_ptr<const MoveEffect>(new MoveEffect(
              effect->m_destination, effect->m_source, effect->m_identity, current, !effect->m_reversed,
              std::move(previous)
          ))
      );
    } catch (const std::exception&) {
      const auto error = currentFileError();
      applied |= changed;
      reply.retireHistory |= invalidates(error);
      result.outcome = changed                     ? TransferOutcome::Partial
          : error.kind == FileErrorKind::Cancelled ? TransferOutcome::Cancelled
                                                   : TransferOutcome::Failed;
      result.error = error;
      stopped = true;
    }
    completed.fetch_add(1);
  }
  std::size_t remaining = 16384;
  if (!stopped) {
    for (std::size_t i = 0; i < folders.size(); ++i) {
      auto& result = reply.results[m_effects.size() + i];
      const auto& folder = folders[i];
      bool changed = false;
      try {
        checkCancelled(token);
        auto next = std::make_shared<MoveFolder>(*folder);
        next->m_item = folder->m_item->refreshFolder(m_undone, remaining, token);
        if (m_undone) {
          next->m_item = next->m_item->storeTo(folder->m_retainingFolder, folder->m_target->get(), changed);
          applied |= changed;
          result.destination = next->m_item->currentLocation();
          result.outcome = TransferOutcome::Moved;
        }
        folders[i] = std::move(next);
      } catch (const std::exception&) {
        applied |= changed;
        result.error = currentFileError();
        if (m_undone) {
          result.outcome = changed                             ? TransferOutcome::Partial
              : result.error->kind == FileErrorKind::Cancelled ? TransferOutcome::Cancelled
                                                               : TransferOutcome::Failed;
        }
        stopped = true;
        break;
      }
      if (m_undone) {
        completed.fetch_add(1);
      }
    }
  }
  if (!stopped) {
    try {
      auto captured = capture(inverse, revisions, folders, !m_undone);
      if (captured) {
        // Serialize old and inverse records too, while retained generations reject stale replay.
        const_cast<MoveReceipt*>(captured.get())->m_mutex = m_mutex;
      }
      reply.receipt = std::move(captured);
    } catch (const FileErrorException&) {
    }
  }
  reply.retireHistory |= applied && !reply.receipt;
  if (reply.retireHistory) {
    retainIncomplete(reply.results);
  }
  if (stopped) {
    for (auto& result : reply.results) {
      if (result.outcome == TransferOutcome::Skipped && !result.error) {
        result.outcome = token.cancelled() ? TransferOutcome::Cancelled : TransferOutcome::Skipped;
        result.error = FileError{
            .kind = token.cancelled() ? FileErrorKind::Cancelled : FileErrorKind::Other,
            .message = "Not attempted after move reversal stopped."
        };
      }
    }
    completed.store(size());
  }
  return reply;
}

void luciole::MoveReceipt::retainIncomplete(std::vector<TransferResult>& results) const {
  const auto append = [](TransferResult& result, const std::vector<Location>& locations) {
    for (const auto& location : locations) {
      if (!result.error) {
        result.error = FileError{.kind = FileErrorKind::Other, .message = "Move history is unavailable."};
      }
      result.error->message += " Inspect retained item: " + location.uri();
    }
  };
  for (std::size_t i = 0; i < m_effects.size(); ++i) {
    append(results[i], m_effects[i]->retainForRecovery());
  }
  for (std::size_t i = 0; i < m_folders.size(); ++i) {
    append(results[m_effects.size() + i], m_folders[i]->retainForRecovery());
  }
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
  for (const auto& folder : m_folders) {
    auto& result = results.emplace_back(folder->m_source, TransferOutcome::Partial, error);
    result.error->message = "Merge reversal could not be confirmed. Inspect the source and adjacent retained folders. "
        + result.error->message;
  }
  return results;
}
