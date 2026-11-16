#include "operations/copied_move_receipt.h"

#include "operations/history_revisions.h"
#include "operations/transfer_staging.h"

#include <algorithm>
#include <fcntl.h>
#include <linux/fs.h>
#include <set>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
  using namespace luciole;
  void check(const CancelToken& token) {
    if (token.cancelled()) {
      throwFileError(FileErrorKind::Cancelled, "Move reversal cancelled.");
    }
  }
  bool invalidates(const FileError& error) {
    return error.kind == FileErrorKind::Changed
        || error.kind == FileErrorKind::Missing
        || error.kind == FileErrorKind::NotDirectory;
  }
  void parentAt(const Location& source, int held) {
    const auto current = checkedFd(::open(source.parent()->nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!sameIdentity(statFd(current.get()), statFd(held))) {
      throwFileError(FileErrorKind::Changed, "A copied Move containing folder changed.");
    }
  }
  void vacant(const Location& source, int parent) {
    struct stat value{};
    if (::fstatat(parent, source.basename()->c_str(), &value, AT_SYMLINK_NOFOLLOW) == 0) {
      throwFileError(FileErrorKind::Exists, "A copied Move return name is occupied.");
    }
    if (errno != ENOENT) {
      throwErrno(errno);
    }
  }
  struct Origin {
    Location location;
    std::shared_ptr<const Fd> parent;
  };
} // namespace
struct luciole::CopiedMoveEffect::State {
  explicit State(Origin original) : origin(std::move(original)) {}
  std::shared_ptr<const CopyEffect> source, destination;
  Origin origin;
  bool published = false, retired = false;
};
std::shared_ptr<const luciole::CopiedMoveEffect> luciole::CopiedMoveEffect::prepare(
    const Location& source, int from, const Location& destination, int target, int stage, bool replacing,
    std::size_t& remaining, const CancelToken& token, const std::shared_ptr<HistoryRevisions>& revisions
) {
  if (statFd(from).st_dev == statFd(target).st_dev) {
    throwFileError(FileErrorKind::Unsupported, "Copied Move history requires separate native filesystems.");
  }
  auto state =
      std::make_shared<State>(Origin{source, std::make_shared<const Fd>(checkedFd(::fcntl(from, F_DUPFD_CLOEXEC, 0)))});
  state->source = CopyEffect::prepareAt(source, from, from, *source.basename(), remaining, token, revisions);
  state->destination = CopyEffect::prepare(destination, target, stage, replacing, remaining, token, revisions);
  return std::shared_ptr<const CopiedMoveEffect>(new CopiedMoveEffect(std::move(state)));
}
void luciole::CopiedMoveEffect::published(int stage) const {
  m_state->destination = m_state->destination->published(stage);
  m_state->published = true;
}
void luciole::CopiedMoveEffect::beforeSourceRemoval(const CancelToken& token) const {
  if (!m_state->published || m_state->retired) {
    throwFileError(FileErrorKind::Changed, "Copied Move publication is incomplete.");
  }
  m_state->destination->validate(token);
  m_state->source->validate(token);
  check(token);
}
void luciole::CopiedMoveEffect::sourceRemoved(const CancelToken& token) const {
  // Release the original payload descriptor so a successful Move frees source space.
  m_state->source.reset();
  parentAt(m_state->origin.location, m_state->origin.parent->get());
  vacant(m_state->origin.location, m_state->origin.parent->get());
  m_state->destination->validate(token);
  m_state->retired = true;
}
std::vector<luciole::Location> luciole::CopiedMoveEffect::retainForRecovery() const {
  auto paths = m_state->source ? m_state->source->retainForRecovery() : std::vector<Location>{};
  auto target = m_state->destination->retainForRecovery();
  paths.insert(paths.end(), target.begin(), target.end());
  return paths;
}
struct luciole::CopiedMoveReceipt::State {
  std::mutex mutex;
  std::shared_ptr<const CopyReceipt> sources, destinations;
  std::vector<std::pair<Location, Location>> locations;
  std::vector<Origin> origins;
  std::vector<std::shared_ptr<const CopyEffect>> restored;
  std::uint64_t generation = 0;
};
luciole::CopiedMoveReceipt::CopiedMoveReceipt(std::shared_ptr<State> state, std::uint64_t generation, bool undone)
    : m_state(std::move(state)), m_generation(generation), m_undone(undone) {}
std::shared_ptr<const luciole::MoveReceipt> luciole::CopiedMoveReceipt::capture(
    const std::vector<std::shared_ptr<const CopiedMoveEffect>>& effects, const CancelToken& token
) {
  if (effects.empty() || effects.size() > 4096) {
    return {};
  }
  std::vector<std::shared_ptr<const CopyEffect>> destinations;
  destinations.reserve(effects.size());
  auto state = std::make_shared<State>();
  state->locations.reserve(effects.size());
  for (const auto& effect : effects) {
    const auto& item = *effect->m_state;
    if (!item.retired || !item.published) {
      throwFileError(FileErrorKind::Changed, "The copied Move proof is incomplete.");
    }
    state->origins.push_back(item.origin);
    destinations.push_back(item.destination);
    state->locations.emplace_back(item.origin.location, item.destination->m_destination);
  }
  state->destinations = CopyReceipt::capture(destinations, TransferAction::Copy, token);
  if (!state->destinations || state->destinations->size() != effects.size()) {
    throwFileError(
        FileErrorKind::Unsupported, "Copied Move history requires independent source and destination roots."
    );
  }
  preflight(*state->destinations, token);
  for (const auto& origin : state->origins) {
    parentAt(origin.location, origin.parent->get());
    vacant(origin.location, origin.parent->get());
  }
  return std::shared_ptr<const MoveReceipt>(new CopiedMoveReceipt(std::move(state), 0, false));
}
std::size_t luciole::CopiedMoveReceipt::size() const { return m_state->locations.size(); }
void luciole::CopiedMoveReceipt::preflight(const CopyReceipt& receipt, const CancelToken& token) {
  if (receipt.m_generation != receipt.m_revisions->generation()) {
    throwFileError(FileErrorKind::Changed, "Copied Move history is stale.");
  }
  for (const auto& effect : receipt.m_effects) {
    check(token);
    effect->validate(token);
    if (effect->m_hidden && !effect->m_previous) {
      effect->vacant();
    }
  }
}
std::vector<luciole::TransferResult>
luciole::CopiedMoveReceipt::results(TransferOutcome outcome, const std::optional<FileError>& error) const {
  std::vector<TransferResult> values;
  values.reserve(size());
  for (const auto& [source, destination] : m_state->locations) {
    values.emplace_back(m_undone ? source : destination, outcome, error);
  }
  return values;
}
void luciole::CopiedMoveReceipt::retain(std::vector<TransferResult>& values) const {
  for (std::size_t i = 0; i < size(); ++i) {
    for (const auto& path : m_state->destinations->m_effects[i]->retainForRecovery()) {
      if (!values[i].error) {
        values[i].error = FileError{FileErrorKind::Other, "Move reversal is incomplete."};
      }
      values[i].error->message += " Inspect retained item: " + path.uri();
    }
    if (values[i].error) {
      values[i].error->message += " Inspect source "
          + m_state->locations[i].first.uri()
          + " and destination "
          + m_state->locations[i].second.uri();
    }
  }
}
void luciole::CopiedMoveReceipt::preflight(const CancelToken& token) const {
  preflight(*m_state->destinations, token);
  if (m_undone) {
    if (!m_state->sources || m_state->restored.size() != size()) {
      throwFileError(FileErrorKind::Changed, "The restored Move source proof is incomplete.");
    }
    preflight(*m_state->sources, token);
  }
  for (const auto& origin : m_state->origins) {
    check(token);
    parentAt(origin.location, origin.parent->get());
    if (!m_undone) {
      vacant(origin.location, origin.parent->get());
    }
  }
}
void luciole::CopiedMoveReceipt::restoreSources(
    const CancelToken& token, MoveReplayResult& reply, bool& applied
) const {
  std::atomic<std::uint64_t> bytes{0};
  transfer::Work work(token, bytes, {});
  work.history = std::make_shared<HistoryRevisions>();
  std::size_t remaining = 16384;
  m_state->restored.assign(size(), {});
  for (std::size_t i = 0; i < size(); ++i) {
    const auto& origin = m_state->origins[i];
    const auto& incoming = m_state->destinations->m_effects[i];
    reply.results[i].outcome = TransferOutcome::Failed;
    check(token);
    incoming->validate(token);
    parentAt(origin.location, origin.parent->get());
    vacant(origin.location, origin.parent->get());
    transfer::Stage stage(origin.parent->get(), work);
    (void)transfer::copyTree(incoming->parent(), *incoming->m_destination.basename(), stage.fd(), "item", work, 0);
    incoming->validate(token);
    if (work.metadataIncomplete) {
      throwFileError(FileErrorKind::Unsupported, "The original filesystem cannot preserve some copied metadata.");
    }
    auto proof =
        CopyEffect::prepare(origin.location, origin.parent->get(), stage.fd(), false, remaining, token, work.history);
    parentAt(origin.location, origin.parent->get());
    vacant(origin.location, origin.parent->get());
    check(token);
    if (::syscall(
            SYS_renameat2, stage.fd(), "item", origin.parent->get(), origin.location.basename()->c_str(),
            RENAME_NOREPLACE
        )
        != 0) {
      throwErrno(errno);
    }
    applied = true;
    reply.results[i].children.emplace_back(incoming->m_destination, TransferOutcome::Copied).destination =
        origin.location;
    m_state->restored[i] = proof;
    m_state->restored[i] = proof->published(stage.fd());
    if (!stage.clear()) {
      throwFileError(FileErrorKind::Other, "The source was restored; inspect its private staging folder.");
    }
  }
  // Source copies must be durable before any destination is removed or exchanged.
  std::set<dev_t> synced;
  for (const auto& origin : m_state->origins) {
    check(token);
    if (synced.insert(statFd(origin.parent->get()).st_dev).second) {
      transfer::synchronize(origin.parent->get());
    }
  }
  m_state->sources = CopyReceipt::capture(m_state->restored, TransferAction::Copy, token);
  if (!m_state->sources) {
    throwFileError(FileErrorKind::Changed, "The restored Move sources could not be verified.");
  }
}
void luciole::CopiedMoveReceipt::removeSources(const CancelToken& token, MoveReplayResult& reply, bool& applied) const {
  std::atomic<std::uint64_t> bytes{0};
  transfer::Work work(token, bytes, {});
  work.history = m_state->restored.front()->revisions();
  std::set<dev_t> synced;
  for (const auto& effect : m_state->destinations->m_effects) {
    check(token);
    effect->validate(token);
    if (synced.insert(statFd(effect->parent()).st_dev).second) {
      transfer::synchronize(effect->parent());
    }
  }
  m_state->sources.reset();
  for (std::size_t i = 0; i < size(); ++i) {
    const auto& origin = m_state->origins[i];
    const auto& proof = m_state->restored[i];
    check(token);
    parentAt(origin.location, origin.parent->get());
    m_state->destinations->m_effects[i]->validate(token);
    proof->validate(token);
    auto tree = *proof->m_tree;
    tree.name = *origin.location.basename();
    // Recursive removal can have partial effects even when it throws.
    applied = true;
    transfer::removeSource(origin.parent->get(), tree, work);
    m_state->restored[i].reset();
    reply.results[i].children.emplace_back(origin.location, TransferOutcome::Moved).destination =
        m_state->locations[i].second;
  }
  m_state->sources.reset();
  m_state->restored.clear();
}
void luciole::CopiedMoveReceipt::reverseDestinations(
    const CancelToken& token, MoveReplayResult& reply, bool& applied
) const {
  const auto before = m_state->destinations->m_revisions->generation();
  std::atomic<std::size_t> completed{0};
  auto phase = m_state->destinations->reverse(token, completed);
  applied |= before != m_state->destinations->m_revisions->generation();
  for (std::size_t i = 0; i < size(); ++i) {
    reply.results[i].children.push_back(phase.results[i]);
    if (phase.results[i].error) {
      reply.results[i].error = phase.results[i].error;
      reply.results[i].outcome = phase.results[i].outcome;
    }
  }
  reply.retireHistory |= phase.retireHistory;
  if (!phase.receipt) {
    for (const auto& result : phase.results) {
      if (result.error) {
        throw FileErrorException(*result.error);
      }
    }
    throwFileError(FileErrorKind::Other, "The destination reversal could not be confirmed.");
  }
  m_state->destinations = std::move(phase.receipt);
}
std::vector<luciole::TransferResult> luciole::CopiedMoveReceipt::unconfirmed(const FileError& error) const {
  auto values = results(TransferOutcome::Partial, error);
  const std::unique_lock lock(m_state->mutex, std::try_to_lock);
  if (lock.owns_lock() && m_generation == m_state->generation) {
    ++m_state->generation;
    retain(values);
  }
  return values;
}
luciole::MoveReplayResult
luciole::CopiedMoveReceipt::reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const {
  MoveReplayResult reply;
  reply.results = results(TransferOutcome::Skipped);
  const std::unique_lock lock(m_state->mutex, std::try_to_lock);
  bool applied = false, began = false;
  try {
    if (!lock.owns_lock()) {
      throwFileError(FileErrorKind::Other, "Another reversal is using this Move history.");
    }
    if (m_generation != m_state->generation) {
      throwFileError(FileErrorKind::Changed, "Copied Move history is stale.");
    }
    preflight(token);
    began = true;
    if (!m_undone) {
      restoreSources(token, reply, applied);
      reverseDestinations(token, reply, applied);
    } else {
      reverseDestinations(token, reply, applied);
      removeSources(token, reply, applied);
    }
    ++m_state->generation;
    for (std::size_t i = 0; i < size(); ++i) {
      reply.results[i].outcome = TransferOutcome::Moved;
      reply.results[i].destination = m_undone ? m_state->locations[i].second : m_state->locations[i].first;
    }
    reply.receipt = std::shared_ptr<const MoveReceipt>(new CopiedMoveReceipt(m_state, m_state->generation, !m_undone));
  } catch (const FileErrorException& failure) {
    const auto& error = failure.error();
    reply.retireHistory |= applied || invalidates(error);
    for (auto& result : reply.results) {
      const bool changed = std::ranges::any_of(result.children, [](const auto& child) {
        return child.outcome == TransferOutcome::Moved
            || child.outcome == TransferOutcome::Copied
            || child.outcome == TransferOutcome::Partial;
      });
      result.outcome = changed                                  ? TransferOutcome::Partial
          : error.kind == FileErrorKind::Cancelled              ? TransferOutcome::Cancelled
          : began && result.outcome == TransferOutcome::Skipped ? TransferOutcome::Skipped
                                                                : TransferOutcome::Failed;
      if (!result.error) {
        result.error = error;
      }
    }
    if (reply.retireHistory && lock.owns_lock() && m_generation == m_state->generation) {
      ++m_state->generation;
      retain(reply.results);
    }
  }
  completed.store(size());
  return reply;
}
