#include "operations/copy_receipt.h"

#include "fs/background_job.h"
#include "fs/filename.h"
#include "fs/posix_fd.h"
#include "operations/copy_history_tree.h"
#include "operations/creation_stage.h"
#include "operations/history_revisions.h"
#include "operations/transfer_copy.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <mutex>
#include <print>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {
  std::atomic<unsigned> pendingCleanup{0};
  std::mutex cleanupMutex;
  std::vector<luciole::FileError> cleanupErrors;
  void check(const luciole::CancelToken& token) {
    if (token.cancelled()) {
      luciole::throwFileError(luciole::FileErrorKind::Cancelled, "Copy reversal cancelled.");
    }
  }
  bool invalidates(const luciole::FileError& error) {
    return error.kind == luciole::FileErrorKind::Changed
        || error.kind == luciole::FileErrorKind::Missing
        || error.kind == luciole::FileErrorKind::NotDirectory;
  }
  luciole::Fd
  currentParent(const luciole::Location& destination, int held, const std::map<std::string, int>& parents = {}) {
    const auto parent = destination.parent();
    if (!parent || !parent->nativePath()) {
      luciole::throwFileError(luciole::FileErrorKind::InvalidArgument, "Copy history requires native folders.");
    }
    if (const auto planned = parents.find(parent->uri()); planned != parents.end()) {
      if (!luciole::sameIdentity(luciole::statFd(planned->second), luciole::statFd(held))) {
        luciole::throwFileError(luciole::FileErrorKind::Changed, "The planned containing folder changed.");
      }
      return luciole::checkedFd(::fcntl(held, F_DUPFD_CLOEXEC, 0));
    }
    auto current = luciole::checkedFd(::open(parent->nativePath()->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!luciole::sameIdentity(luciole::statFd(current.get()), luciole::statFd(held))) {
      luciole::throwFileError(
          luciole::FileErrorKind::Changed, "The containing folder changed. Copy history is no longer valid."
      );
    }
    return current;
  }
  void requireRevision(const struct stat& expected, const struct stat& actual) {
    if (!luciole::sameFileRevision(expected, actual)) {
      luciole::throwFileError(
          luciole::FileErrorKind::Changed, "The copied item or its contents changed. Copy history is no longer valid."
      );
    }
  }
  luciole::transfer::Tree captureTree(
      int parent, const std::string& name, std::size_t& remaining, const luciole::CancelToken& token, dev_t device,
      unsigned depth
  ) {
    check(token);
    if (!remaining || depth > 64) {
      luciole::throwFileError(luciole::FileErrorKind::Unsupported, "Copy history exceeds its tree limit.");
    }
    --remaining;
    luciole::transfer::Tree tree{.name = name, .metadata = luciole::transfer::info(parent, name), .children = {}};
    if (tree.metadata.st_dev != device) {
      luciole::throwFileError(luciole::FileErrorKind::Unsupported, "Copy history cannot cross a mounted subtree.");
    }
    if (S_ISDIR(tree.metadata.st_mode)) {
      auto directory = luciole::transfer::directory(parent, name);
      requireRevision(tree.metadata, luciole::statFd(directory.get()));
      for (const auto& child : luciole::transfer::children(directory.get(), remaining)) {
        tree.children.push_back(captureTree(directory.get(), child, remaining, token, device, depth + 1));
      }
      requireRevision(tree.metadata, luciole::statFd(directory.get()));
    } else if (!S_ISREG(tree.metadata.st_mode) && !S_ISLNK(tree.metadata.st_mode)) {
      luciole::throwFileError(luciole::FileErrorKind::Unsupported, "Copy history does not support special files.");
    }
    return tree;
  }
  void verify(
      int parent, const std::string& name, const luciole::transfer::Tree& tree, const luciole::CancelToken& token,
      const luciole::HistoryRevisions& revisions
  ) {
    check(token);
    revisions.validate(tree.metadata, luciole::transfer::info(parent, name));
    if (S_ISDIR(tree.metadata.st_mode)) {
      auto directory = luciole::transfer::directory(parent, name);
      revisions.validate(tree.metadata, luciole::statFd(directory.get()));
      if (luciole::transfer::children(directory.get(), tree.children.size() + 1).size() != tree.children.size()) {
        luciole::throwFileError(luciole::FileErrorKind::Changed, "The copied folder's contents changed.");
      }
      for (const auto& child : tree.children) {
        verify(directory.get(), child.name, child, token, revisions);
      }
      revisions.validate(tree.metadata, luciole::statFd(directory.get()));
    }
  }

} // namespace

struct luciole::CopyStorage {
  std::mutex replayMutex;
  Fd parent, item;
  std::shared_ptr<HistoryRevisions> revisions;
  std::shared_ptr<const transfer::Tree> cleanupTree;
  std::unique_ptr<CreationStage> stage;
  bool retainForInspection = false;
  std::uint64_t generation = 0;
  ~CopyStorage() {
    if (!stage) {
      return;
    }
    const auto guard = revisions->lock();
    try {
      if (retainForInspection) {
        throwFileError(FileErrorKind::Other, "An incomplete reversal retained this item for recovery.");
      }
      stage->validateItem();
      if (!cleanupTree) {
        throwFileError(FileErrorKind::Changed, "The retained item could not be verified.");
      }
      auto tree = *cleanupTree;
      const auto normalize = [&](auto&& self, transfer::Tree& node) -> void {
        node.metadata = revisions->expected(node.metadata);
        for (auto& child : node.children) {
          self(self, child);
        }
      };
      normalize(normalize, tree);
      verify(stage->fd(), "item", tree, {}, *revisions);
      std::atomic<std::uint64_t> bytes{0};
      transfer::CopyWork work({}, bytes);
      work.history = revisions;
      transfer::removeSource(stage->fd(), tree, work);
      if (stage->clear()) {
        return;
      }
      throwFileError(FileErrorKind::Other, "The private folder could not be removed.");
    } catch (const std::exception& error) {
      auto message =
          "Retained item needs inspection at " + filenameDisplayText(stage->recoveryPath()) + ": " + error.what();
      std::println(stderr, "[file-history] {}", message);
      const std::scoped_lock lock(cleanupMutex);
      if (cleanupErrors.size() == 12) {
        cleanupErrors.erase(cleanupErrors.begin());
      }
      cleanupErrors.push_back({.kind = FileErrorKind::Other, .message = std::move(message)});
    }
  }
};

luciole::CopyEffect::CopyEffect(
    Location destination, std::shared_ptr<CopyStorage> storage, std::shared_ptr<const transfer::Tree> tree, bool hidden,
    std::shared_ptr<const CopyEffect> previous
)
    : m_destination(std::move(destination)), m_storage(std::move(storage)), m_tree(std::move(tree)), m_hidden(hidden),
      m_generation(m_storage->generation), m_previous(std::move(previous)) {}

std::shared_ptr<const luciole::CopyEffect> luciole::CopyEffect::prepare(
    const Location& destination, int parent, int stage, bool replacing, std::size_t& remaining,
    const CancelToken& token, std::shared_ptr<HistoryRevisions> revisions
) {
  if (!revisions) {
    revisions = std::make_shared<HistoryRevisions>();
  }
  const auto guard = revisions->lock();
  auto incoming = prepareAt(destination, parent, stage, "item", remaining, token, revisions);
  auto previous = replacing
      ? prepareAt(destination, parent, parent, *destination.basename(), remaining, token, revisions)
      : nullptr;
  return std::shared_ptr<const CopyEffect>(
      new CopyEffect(destination, incoming->m_storage, incoming->m_tree, false, std::move(previous))
  );
}

std::shared_ptr<const luciole::CopyEffect> luciole::CopyEffect::prepareAt(
    const Location& destination, int parent, int from, const std::string& name, std::size_t& remaining,
    const CancelToken& token, std::shared_ptr<HistoryRevisions> revisions
) {
  const auto guard = revisions->lock();
  (void)currentParent(destination, parent);
  auto item = checkedFd(::openat(from, name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
  auto tree = std::make_shared<transfer::Tree>(captureTree(from, name, remaining, token, statFd(item.get()).st_dev, 0));
  tree->name = "item";
  revisions->validate(tree->metadata, statFd(item.get()));
  const auto remember = [&](auto&& self, const transfer::Tree& node) -> void {
    revisions->capture(node.metadata);
    for (const auto& child : node.children) {
      self(self, child);
    }
  };
  remember(remember, *tree);
  auto owned = std::make_unique<CopyStorage>();
  owned->revisions = std::move(revisions);
  owned->parent = checkedFd(::fcntl(parent, F_DUPFD_CLOEXEC, 0));
  owned->item = std::move(item);
  std::shared_ptr<CopyStorage> storage(owned.release(), [worker = std::this_thread::get_id()](CopyStorage* retired) {
    auto value = std::unique_ptr<CopyStorage>(retired);
    if (std::this_thread::get_id() == worker && !value->stage) {
      return;
    }
    ++pendingCleanup;
    detail::runInThread([value = std::move(value)] mutable { value.reset(); }, [] { --pendingCleanup; });
  });
  return std::shared_ptr<const CopyEffect>(new CopyEffect(destination, std::move(storage), std::move(tree), false));
}

void luciole::CopyEffect::validate(const CancelToken& token, const std::map<std::string, int>& parents) const {
  (void)currentParent(m_destination, m_storage->parent.get(), parents);
  if (m_hidden != static_cast<bool>(m_storage->stage) || m_generation != m_storage->generation) {
    throwFileError(FileErrorKind::Changed, "The copy history entry is stale.");
  }
  revisions()->validate(m_tree->metadata, statFd(m_storage->item.get()));
  if (m_hidden) {
    m_storage->stage->validateItem();
    verify(m_storage->stage->fd(), "item", *m_tree, token, *revisions());
  } else {
    verify(m_storage->parent.get(), *m_destination.basename(), *m_tree, token, *revisions());
  }
  if (m_previous) {
    m_previous->validate(token, parents);
  }
}

bool luciole::CopyEffect::ownsProof() const {
  return m_generation == m_storage->generation
      && m_hidden == static_cast<bool>(m_storage->stage)
      && (!m_previous || m_previous->ownsProof());
}

void luciole::CopyEffect::vacant() const {
  struct stat value{};
  if (::fstatat(m_storage->parent.get(), m_destination.basename()->c_str(), &value, AT_SYMLINK_NOFOLLOW) == 0) {
    throwFileError(
        FileErrorKind::Exists, "The destination name is occupied: " + m_destination.displayName().value_or("Item") + "."
    );
  }
  if (errno != ENOENT) {
    throwErrno(errno);
  }
}

std::shared_ptr<const luciole::CopyEffect> luciole::CopyEffect::afterMove(bool hidden) const {
  auto tree = std::make_shared<transfer::Tree>(*m_tree);
  const auto current = statFd(m_storage->item.get());
  revisions()->renamed(tree->metadata, current);
  tree->metadata = current;
  ++m_storage->generation;
  auto result = std::shared_ptr<const CopyEffect>(new CopyEffect(m_destination, m_storage, tree, hidden));
  result->validate({});
  if (hidden) {
    m_storage->cleanupTree = std::move(tree);
  }
  return result;
}
std::shared_ptr<const luciole::CopyEffect> luciole::CopyEffect::published(int from, int retentionParent) const {
  auto incoming = afterMove(false);
  if (!m_previous) {
    return incoming;
  }
  return std::shared_ptr<const CopyEffect>(new CopyEffect(
      m_destination, m_storage, incoming->m_tree, false, m_previous->retainedFrom(from, "item", retentionParent)
  ));
}

std::shared_ptr<luciole::HistoryRevisions> luciole::CopyEffect::revisions() const { return m_storage->revisions; }
void luciole::CopyEffect::verifyAt(const CopyEffect& actual, const CancelToken& token) const {
  if (!sameIdentity(statFd(item()), statFd(actual.item()))) {
    throwFileError(FileErrorKind::Changed, "A repeated destination does not contain the recorded version.");
  }
  actual.validate(token);
  verify(
      actual.m_storage->stage ? actual.m_storage->stage->fd() : actual.parent(),
      actual.m_storage->stage ? "item" : *actual.m_destination.basename(), *m_tree, token, *revisions()
  );
}
int luciole::CopyEffect::item() const { return m_storage->item.get(); }
luciole::Location luciole::CopyEffect::currentLocation() const {
  return m_storage->stage ? Location::fromPath(m_storage->stage->recoveryPath()) : m_destination;
}
int luciole::CopyEffect::parent() const { return m_storage->parent.get(); }
std::mutex& luciole::CopyEffect::mutex() const { return m_storage->replayMutex; }

std::shared_ptr<const luciole::CopyEffect>
luciole::CopyEffect::retainedFrom(int from, const std::string& name, int retentionParent) const {
  auto& storage = *m_storage;
  try {
    auto expected = *m_tree;
    revisions()->renamed(expected.metadata, statFd(item()));
    expected.metadata = revisions()->expected(expected.metadata);
    verify(from, name, expected, {}, *revisions());
    auto stage = std::make_unique<CreationStage>(retentionParent >= 0 ? retentionParent : parent(), false);
    stage->adopt(checkedFd(::fcntl(item(), F_DUPFD_CLOEXEC, 0)));
    (void)currentParent(m_destination, parent());
    verify(from, name, expected, {}, *revisions());
    if (::syscall(SYS_renameat2, from, name.c_str(), stage->fd(), "item", RENAME_NOREPLACE) != 0) {
      throwErrno(errno);
    }
    storage.stage = std::move(stage);
    return afterMove(true);
  } catch (const std::exception&) {
    auto error = currentFileError();
    if (storage.stage) {
      storage.retainForInspection = true;
      error.message += " Inspect retained item: " + filenameDisplayText(storage.stage->recoveryPath());
    }
    throw FileErrorException(std::move(error));
  }
}

std::shared_ptr<const luciole::CopyEffect>
luciole::CopyEffect::restoreTo(const Location& destination, int target, bool& changed) const {
  validate({});
  if (!m_hidden || m_previous) {
    throwFileError(FileErrorKind::Changed, "Invalid retained-item restoration.");
  }
  (void)currentParent(destination, target);
  auto nextParent = checkedFd(::fcntl(target, F_DUPFD_CLOEXEC, 0));
  if (::syscall(
          SYS_renameat2, m_storage->stage->fd(), "item", target, destination.basename()->c_str(), RENAME_NOREPLACE
      )
      != 0) {
    throwErrno(errno);
  }
  changed = true;
  auto stage = std::move(m_storage->stage);
  m_storage->cleanupTree.reset();
  if (!stage->clear()) {
    throwFileError(
        FileErrorKind::Other,
        "The item returned, but inspect its private folder: " + filenameDisplayText(stage->recoveryPath())
    );
  }
  m_storage->parent = std::move(nextParent);
  auto moved = std::shared_ptr<const CopyEffect>(new CopyEffect(destination, m_storage, m_tree, false));
  return moved->afterMove(false);
}

std::shared_ptr<const luciole::CopyEffect>
luciole::CopyEffect::storeTo(const Location& folder, int target, bool& changed, int retentionParent) const {
  validate({});
  if (m_hidden || m_previous) {
    throwFileError(FileErrorKind::Changed, "Invalid retained-item storage.");
  }
  const auto destination = folder.child(*m_destination.basename());
  (void)currentParent(destination, target);
  auto nextParent = checkedFd(::fcntl(target, F_DUPFD_CLOEXEC, 0));
  auto stage = std::make_unique<CreationStage>(retentionParent >= 0 ? retentionParent : nextParent.get(), false);
  stage->adopt(checkedFd(::fcntl(item(), F_DUPFD_CLOEXEC, 0)));
  validate({});
  if (::syscall(SYS_renameat2, parent(), m_destination.basename()->c_str(), stage->fd(), "item", RENAME_NOREPLACE)
      != 0) {
    throwErrno(errno);
  }
  changed = true;
  m_storage->parent = std::move(nextParent);
  m_storage->stage = std::move(stage);
  auto moved = std::shared_ptr<const CopyEffect>(new CopyEffect(destination, m_storage, m_tree, true));
  return moved->afterMove(true);
}

std::shared_ptr<const luciole::CopyEffect>
luciole::CopyEffect::refreshFolder(bool empty, std::size_t& remaining, const CancelToken& token) const {
  if (m_hidden || m_previous || !S_ISDIR(m_tree->metadata.st_mode) || !ownsProof()) {
    throwFileError(FileErrorKind::Changed, "Invalid merged-folder history.");
  }
  (void)currentParent(m_destination, parent());
  const auto current = statFd(item());
  // Only directory-entry bookkeeping may change as recorded children move in or out.
  revisions()->directoryChanged(m_tree->metadata, current);
  auto tree = std::make_shared<transfer::Tree>(
      captureTree(parent(), *m_destination.basename(), remaining, token, current.st_dev, 0)
  );
  requireRevision(current, tree->metadata);
  if (empty && !tree->children.empty()) {
    throwFileError(FileErrorKind::Changed, "New source entries remain in a merged folder.");
  }
  tree->name = "item";
  return std::shared_ptr<const CopyEffect>(new CopyEffect(m_destination, m_storage, std::move(tree), false));
}

std::shared_ptr<const luciole::CopyReceipt> luciole::CopyReceipt::capture(
    const std::vector<std::shared_ptr<const CopyEffect>>& effects, TransferAction action, const CancelToken& token
) {
  if (effects.empty() || effects.size() > 4096 || action == TransferAction::Move) {
    return {};
  }
  auto result = std::make_shared<CopyReceipt>();
  result->m_action = action;
  result->m_revisions = effects.front()->revisions();
  const auto guard = result->m_revisions->lock();
  std::map<std::string, std::size_t> latest;
  std::vector<std::unique_ptr<CopyHistoryTree>> trees;
  for (const auto& effect : effects) {
    check(token);
    if (effect->revisions() != result->m_revisions) {
      throwFileError(FileErrorKind::Changed, "Copy history belongs to different jobs.");
    }
    if (const auto found = latest.find(effect->m_destination.uri()); found != latest.end()) {
      const auto index = found->second;
      const auto& prior = result->m_effects[index];
      if (!effect->m_previous || effect->m_hidden || prior->m_hidden) {
        throwFileError(FileErrorKind::Changed, "Invalid repeated copy destination.");
      }
      if (trees[index]) {
        effect->m_previous->validate(token);
        trees[index]->matches(*effect->m_previous->m_tree, *result->m_revisions, token);
        trees[index].reset();
      } else {
        prior->verifyAt(*effect->m_previous, token);
      }
      result->m_effects[index] = std::shared_ptr<const CopyEffect>(
          new CopyEffect(effect->m_destination, effect->m_storage, effect->m_tree, false, prior->m_previous)
      );
      continue;
    }
    std::vector<std::string> path{*effect->m_destination.basename()};
    auto parent = effect->m_destination.parent();
    bool absorbed = false;
    while (parent && path.size() <= 64) {
      if (const auto found = latest.find(parent->uri()); found != latest.end()) {
        const auto index = found->second;
        const auto& ancestor = result->m_effects[index];
        if (action != TransferAction::Copy || effect->m_hidden || ancestor->m_hidden || !ancestor->ownsProof()) {
          throwFileError(FileErrorKind::Changed, "Invalid overlapping copy destination.");
        }
        if (effect->m_previous) {
          effect->m_previous->validate(token);
        }
        if (!trees[index]) {
          trees[index] = std::make_unique<CopyHistoryTree>(*ancestor->m_tree);
        }
        std::ranges::reverse(path);
        trees[index]->replace(
            path, *effect->m_tree, effect->m_previous ? effect->m_previous->m_tree.get() : nullptr,
            *result->m_revisions, token
        );
        absorbed = true;
        break;
      }
      const auto name = parent->basename();
      if (!name) {
        break;
      }
      path.push_back(*name);
      parent = parent->parent();
    }
    if (!absorbed) {
      latest.emplace(effect->m_destination.uri(), result->m_effects.size());
      result->m_effects.push_back(effect);
      trees.push_back({});
    }
  }
  for (std::size_t index = 0; index < result->m_effects.size(); ++index) {
    auto& effect = result->m_effects[index];
    if (trees[index]) {
      auto tree = std::make_shared<transfer::Tree>(
          trees[index]->finish(effect->parent(), *effect->m_destination.basename(), *result->m_revisions, token)
      );
      effect = std::shared_ptr<const CopyEffect>(new CopyEffect(
          effect->m_destination, effect->m_storage, std::move(tree), effect->m_hidden, effect->m_previous
      ));
    }
    effect->validate(token);
  }
  result->m_generation = result->m_revisions->generation();
  return result;
}

luciole::CopyReplayResult
luciole::CopyReceipt::reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const {
  const auto revisionGuard = m_revisions->lock();
  CopyReplayResult reply;
  reply.results.reserve(size());
  for (const auto& effect : m_effects) {
    reply.results.emplace_back(effect->m_destination, TransferOutcome::Skipped);
  }
  std::vector<std::unique_lock<std::mutex>> guards;
  bool ownsProof = false;
  try {
    // Replays from independently constructed service instances must not race mutable retained storage.
    // Lock every root before preflight, without waiting for another replay.
    guards.reserve(size() * 2);
    for (const auto& effect : m_effects) {
      for (const auto* item : {effect.get(), effect->m_previous.get()}) {
        if (item) {
          guards.emplace_back(item->m_storage->replayMutex, std::try_to_lock);
          if (!guards.back().owns_lock()) {
            throwFileError(FileErrorKind::Other, "Another copy reversal is already using this history.");
          }
        }
      }
    }
    ownsProof = m_generation == m_revisions->generation()
        && std::ranges::all_of(m_effects, [](const auto& effect) { return effect->ownsProof(); });
    if (!ownsProof) {
      throwFileError(FileErrorKind::Changed, "The copy history entry is stale.");
    }
    for (const auto& effect : m_effects) {
      check(token);
      effect->validate(token);
      if (effect->m_hidden && !effect->m_previous) {
        effect->vacant();
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
  std::vector<std::shared_ptr<const CopyEffect>> inverse;
  bool stopped = false, applied = false;
  for (std::size_t i = 0; i < size(); ++i) {
    auto& result = reply.results[i];
    const auto& effect = m_effects[i];
    auto& storage = *effect->m_storage;
    if (stopped) {
      result.outcome = token.cancelled() ? TransferOutcome::Cancelled : TransferOutcome::Skipped;
      result.error = FileError{
          .kind = token.cancelled() ? FileErrorKind::Cancelled : FileErrorKind::Other,
          .message = "Not attempted after copy reversal stopped."
      };
      completed.fetch_add(1);
      continue;
    }
    bool moved = false;
    try {
      effect->validate(token);
      check(token);
      if (effect->m_previous) {
        auto& previous = *effect->m_previous->m_storage;
        const auto retainedPath = Location::fromPath(previous.stage->recoveryPath());
        previous.stage->exchange(*effect->m_destination.basename(), storage.parent.get(), storage.item.get());
        moved = applied = true;
        storage.stage = std::move(previous.stage);
        previous.cleanupTree.reset();
        storage.stage->adopt(checkedFd(::fcntl(storage.item.get(), F_DUPFD_CLOEXEC, 0)));
        result.outcome = TransferOutcome::Moved;
        result.destination = effect->m_destination;
        result.children.emplace_back(effect->m_destination, TransferOutcome::Moved).destination = retainedPath;
        result.children.emplace_back(retainedPath, TransferOutcome::Moved).destination = effect->m_destination;
        auto retained = effect->afterMove(true);
        auto restored = effect->m_previous->afterMove(false);
        inverse.push_back(
            std::shared_ptr<const CopyEffect>(new CopyEffect(
                restored->m_destination, restored->m_storage, restored->m_tree, false, std::move(retained)
            ))
        );
      } else if (effect->m_hidden) {
        effect->vacant();
        storage.stage->publish(*effect->m_destination.basename());
        moved = applied = true;
        result.destination = effect->m_destination;
        result.outcome = m_action == TransferAction::Link ? TransferOutcome::Linked : TransferOutcome::Copied;
        auto stage = std::move(storage.stage);
        storage.cleanupTree.reset();
        if (!stage->clear()) {
          throwFileError(
              FileErrorKind::Other,
              "The copy returned, but inspect its private folder: " + filenameDisplayText(stage->recoveryPath())
          );
        }
        inverse.push_back(effect->afterMove(false));
      } else {
        auto stage = std::make_unique<CreationStage>(storage.parent.get(), false);
        stage->adopt(checkedFd(::fcntl(storage.item.get(), F_DUPFD_CLOEXEC, 0)));
        effect->validate(token);
        check(token);
        if (::syscall(
                SYS_renameat2, storage.parent.get(), effect->m_destination.basename()->c_str(), stage->fd(), "item",
                RENAME_NOREPLACE
            )
            != 0) {
          throwErrno(errno);
        }
        moved = applied = true;
        storage.stage = std::move(stage);
        result.destination = Location::fromPath(storage.stage->recoveryPath());
        result.outcome = TransferOutcome::Moved;
        inverse.push_back(effect->afterMove(true));
      }
    } catch (const std::exception&) {
      auto error = currentFileError();
      reply.retireHistory |= invalidates(error);
      result.outcome = moved                       ? TransferOutcome::Partial
          : error.kind == FileErrorKind::Cancelled ? TransferOutcome::Cancelled
                                                   : TransferOutcome::Failed;
      if (storage.stage) {
        error.message += " Inspect retained copy: " + filenameDisplayText(storage.stage->recoveryPath());
      }
      result.error = std::move(error);
      stopped = true;
    }
    completed.fetch_add(1);
  }
  if (applied) {
    m_revisions->advance();
  }
  if (!stopped) {
    try {
      reply.receipt = capture(inverse, m_action, {});
    } catch (const FileErrorException&) {
      // Committed removals/publications remain accurately reported if optional inverse capture fails.
    }
  }
  reply.retireHistory |= applied && !reply.receipt;
  if (reply.retireHistory) {
    retainIncomplete(reply.results);
  }
  return reply;
}

void luciole::CopyReceipt::retainIncomplete(std::vector<TransferResult>& results) const {
  for (std::size_t i = 0; i < size(); ++i) {
    for (const auto& location : m_effects[i]->retainForRecovery()) {
      results[i].destination = location;
      if (!results[i].error) {
        results[i].error =
            FileError{.kind = FileErrorKind::Other, .message = "Copy retained after incomplete reversal."};
      }
      results[i].error->message += " Inspect retained copy: " + location.uri();
    }
  }
}

std::vector<luciole::Location> luciole::CopyEffect::retainForRecovery() const {
  std::vector<Location> retained;
  for (const auto* item : {this, m_previous.get()}) {
    if (item && item->m_storage->stage) {
      item->m_storage->retainForInspection = true;
      retained.push_back(Location::fromPath(item->m_storage->stage->recoveryPath()));
    }
  }
  return retained;
}

std::vector<luciole::TransferResult> luciole::CopyReceipt::unconfirmed(const FileError& error) const {
  std::vector<TransferResult> results;
  for (const auto& effect : m_effects) {
    auto& result = results.emplace_back(effect->m_destination, TransferOutcome::Partial, error);
    result.error->message = "Copy reversal could not be confirmed. Inspect the destination. " + result.error->message;
    result.error->message += " Also inspect adjacent .luciole-create-* folders for retained items.";
  }
  return results;
}
bool luciole::CopyReceipt::cleanupPending() { return pendingCleanup.load() != 0; }
std::vector<luciole::FileError> luciole::CopyReceipt::takeCleanupErrors() {
  const std::scoped_lock lock(cleanupMutex);
  return std::exchange(cleanupErrors, {});
}
