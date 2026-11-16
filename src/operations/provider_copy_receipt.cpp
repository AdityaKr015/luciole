#include "operations/provider_copy_receipt.h"

#include "fs/background_job.h"
#include "fs/transfer_types.h"
#include "operations/provider_transfer.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <print>
#include <utility>

namespace {
  using namespace luciole;
  namespace ph = luciole::provider_history;
  std::atomic<unsigned> pendingCleanup{0};
  std::mutex cleanupMutex;
  std::vector<FileError> cleanupErrors;
  constexpr const char* kMarker = "history-token";

  // Cleanup is best effort; request cancellation rather than holding shutdown indefinitely on a lost server.
  class CleanupDeadline {
  public:
    CleanupDeadline() : m_source(g_timeout_source_new_seconds(5)) {
      g_source_set_callback(
          m_source,
          [](gpointer data) -> gboolean {
            static_cast<CancelToken*>(data)->cancel();
            return G_SOURCE_REMOVE;
          },
          new CancelToken(token), [](gpointer data) { delete static_cast<CancelToken*>(data); }
      );
      g_source_attach(m_source, nullptr);
    }
    ~CleanupDeadline() {
      g_source_destroy(m_source);
      g_source_unref(m_source);
    }
    CleanupDeadline(const CleanupDeadline&) = delete;
    CleanupDeadline& operator=(const CleanupDeadline&) = delete;
    CancelToken token;

  private:
    GSource* m_source;
  };
  [[noreturn]] void fail(GError* raw) {
    const GErrorPtr error(raw);
    throw FileErrorException(error ? fileError(*error) : FileError{FileErrorKind::Other, "Remote history failed."});
  }
  void createDirectory(const Location& location, const CancelToken& token) {
    GError* error = nullptr;
    if (!g_file_make_directory(gioFile(location).get(), gioCancellable(token), &error)) {
      fail(error);
    }
  }
  void restrictDirectory(const Location& location, const CancelToken& token) {
    GError* error = nullptr;
    if (!g_file_set_attribute_uint32(
            gioFile(location).get(), G_FILE_ATTRIBUTE_UNIX_MODE, 0700, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
            gioCancellable(token), &error
        )) {
      fail(error);
    }
    const auto value = ph::info(location, token);
    if (readIsSymlink(value.get())
        || g_file_info_get_file_type(value.get()) != G_FILE_TYPE_DIRECTORY
        || !g_file_info_has_attribute(value.get(), G_FILE_ATTRIBUTE_UNIX_MODE)
        || (g_file_info_get_attribute_uint32(value.get(), G_FILE_ATTRIBUTE_UNIX_MODE) & 0777U) != 0700U) {
      throwFileError(FileErrorKind::Unsupported, "The provider cannot create private copy history storage.");
    }
  }
  void writeMarker(const Location& location, const std::string& nonce, const CancelToken& token) {
    GError* error = nullptr;
    const GioPtr<GFileOutputStream> file(
        g_file_create(gioFile(location).get(), G_FILE_CREATE_PRIVATE, gioCancellable(token), &error)
    );
    if (!file) {
      fail(error);
    }
    gsize written = 0;
    if (!g_output_stream_write_all(
            G_OUTPUT_STREAM(file.get()), nonce.data(), nonce.size(), &written, gioCancellable(token), &error
        )) {
      fail(error);
    }
    if (!g_output_stream_close(G_OUTPUT_STREAM(file.get()), gioCancellable(token), &error)) {
      fail(error);
    }
    if (written != nonce.size()) {
      throwFileError(FileErrorKind::Other, "Remote history marker write was incomplete.");
    }
  }
  void marker(const Location& anchor, const std::string& nonce, const CancelToken& token) {
    const auto folder = ph::info(anchor, token);
    if (readIsSymlink(folder.get())
        || g_file_info_get_file_type(folder.get()) != G_FILE_TYPE_DIRECTORY
        || (g_file_info_get_attribute_uint32(folder.get(), G_FILE_ATTRIBUTE_UNIX_MODE) & 0777U) != 0700U) {
      throwFileError(FileErrorKind::Changed, "Remote history storage changed.");
    }
    const auto path = anchor.child(kMarker);
    const auto value = ph::info(path, token);
    if (readIsSymlink(value.get())
        || g_file_info_get_file_type(value.get()) != G_FILE_TYPE_REGULAR
        || g_file_info_get_size(value.get()) != static_cast<goffset>(nonce.size())) {
      throwFileError(FileErrorKind::Changed, "Remote history marker changed.");
    }
    GError* error = nullptr;
    const GioPtr<GFileInputStream> stream(g_file_read(gioFile(path).get(), gioCancellable(token), &error));
    if (!stream) {
      fail(error);
    }
    std::string actual(nonce.size() + 1, '\0');
    gsize count = 0;
    if (!g_input_stream_read_all(
            G_INPUT_STREAM(stream.get()), actual.data(), actual.size(), &count, gioCancellable(token), &error
        )) {
      fail(error);
    }
    if (!g_input_stream_close(G_INPUT_STREAM(stream.get()), gioCancellable(token), &error)) {
      fail(error);
    }
    actual.resize(count);
    if (actual != nonce) {
      throwFileError(FileErrorKind::Changed, "Remote history marker changed.");
    }
  }
  void vacant(const Location& location, const CancelToken& token) {
    if (ph::info(location, token, true)) {
      throwFileError(FileErrorKind::Exists, "The remote destination name is occupied: " + location.uri());
    }
  }
  std::string_view attribute(const ph::Tree& tree, const char* name) {
    const auto found = tree.attributes.find(name);
    return found == tree.attributes.end() ? std::string_view{} : found->second;
  }
  bool sameContents(const ph::Tree& first, const ph::Tree& second) {
    if (first.name != second.name
        || first.directory != second.directory
        || first.digest != second.digest
        || first.children.size() != second.children.size()
        || attribute(first, G_FILE_ATTRIBUTE_STANDARD_TYPE) != attribute(second, G_FILE_ATTRIBUTE_STANDARD_TYPE)
        || attribute(first, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET)
            != attribute(second, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET)) {
      return false;
    }
    for (std::size_t i = 0; i < first.children.size(); ++i) {
      if (!sameContents(first.children[i], second.children[i])) {
        return false;
      }
    }
    return true;
  }
  Location copyItem(const Location& source, const Location& parent, const CancelToken& token) {
    ProviderTransfer transfer({{source}, parent, TransferAction::Copy});
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::size_t> completed{0};
    const auto results =
        transfer.run(token, bytes, completed, [](const TransferConflict&) { return TransferConflictChoice::Cancel; });
    if (results.size() != 1 || results.front().outcome != TransferOutcome::Copied || !results.front().destination) {
      if (!results.empty() && results.front().error) {
        throw FileErrorException(*results.front().error);
      }
      throwFileError(FileErrorKind::Other, "Copy history could not publish its retained data.");
    }
    return *results.front().destination;
  }
  struct DirectoryProof {
    Location location;
    std::map<std::string, std::string> attributes;
  };
  DirectoryProof directoryProof(const Location& location, const CancelToken& token) {
    const auto value = ph::info(location, token);
    if (readIsSymlink(value.get()) || g_file_info_get_file_type(value.get()) != G_FILE_TYPE_DIRECTORY) {
      throwFileError(FileErrorKind::Changed, "A remote copy containing folder changed.");
    }
    DirectoryProof proof{location, {}};
    // Membership and modification times change through our own effects and unrelated sibling additions.
    for (const char* attribute :
         {G_FILE_ATTRIBUTE_ID_FILE, G_FILE_ATTRIBUTE_ID_FILESYSTEM, G_FILE_ATTRIBUTE_UNIX_MODE,
          G_FILE_ATTRIBUTE_UNIX_UID, G_FILE_ATTRIBUTE_UNIX_GID}) {
      if (g_file_info_has_attribute(value.get(), attribute)) {
        const luciole::GString text(g_file_info_get_attribute_as_string(value.get(), attribute));
        proof.attributes.emplace(attribute, text.get());
      }
    }
    return proof;
  }
  std::vector<DirectoryProof>
  containingFolders(const Location& destination, const std::optional<Location>& root, const CancelToken& token) {
    std::vector<DirectoryProof> result;
    for (auto current = destination.parent(); current && result.size() < 129; current = current->parent()) {
      result.push_back(directoryProof(*current, token));
      if (!root || *current == *root) {
        std::ranges::reverse(result);
        return result;
      }
    }
    throwFileError(FileErrorKind::Unsupported, "Remote copy history requires bounded containing folders.");
  }
  bool invalidates(const FileError& error) {
    return error.kind == FileErrorKind::Changed
        || error.kind == FileErrorKind::Missing
        || error.kind == FileErrorKind::NotDirectory
        || error.kind == FileErrorKind::Unsupported;
  }
} // namespace

struct luciole::ProviderCopyStorage {
  Location destination, anchor;
  std::string nonce, payloadName;
  provider_history::Tree tree;
  std::vector<DirectoryProof> parents;
  std::optional<provider_history::Tree> previous;
  bool previousStored = false;
  bool local = false, localItemCreated = false, localPreviousCreated = false;
  std::optional<ph::Tree> localTree, localPrevious;
  std::optional<DirectoryProof> localItemParent, localPreviousParent;
  bool anchorAttempted = false, anchorCreated = false, markerReady = false, published = false, hidden = false,
       retain = false;
  ProviderCopyStorage(
      Location target, Location storage, std::string secret, provider_history::Tree proof,
      std::optional<provider_history::Tree> predecessor, std::vector<DirectoryProof> containing, std::string name
  )
      : destination(std::move(target)), anchor(std::move(storage)), nonce(std::move(secret)),
        payloadName(std::move(name)), tree(std::move(proof)), parents(std::move(containing)),
        previous(std::move(predecessor)) {}

  Location current() const { return hidden ? (local ? payload(false) : anchor.child("item")) : destination; }
  Location payload(bool predecessor) const {
    return anchor.child(predecessor ? "previous" : "item").child(payloadName);
  }
  void storeLocal(const Location& source, const ph::Tree& proof, bool predecessor, const CancelToken& token) {
    const auto container = anchor.child(predecessor ? "previous" : "item");
    createDirectory(container, token);
    (predecessor ? localPreviousCreated : localItemCreated) = true;
    (predecessor ? localPreviousParent : localItemParent) = directoryProof(container, token);
    const auto copied = copyItem(source, container, token), target = payload(predecessor);
    if (copied != target) {
      ph::move(copied, target, token);
    }
    ph::Budget budget;
    auto retained = ph::capture(target, budget, token);
    if (!sameContents(proof, retained)) {
      throwFileError(FileErrorKind::Changed, "Retained copy contents differ from the captured provider data.");
    }
    ph::verify(source, proof, token);
    (predecessor ? localPrevious : localTree) = std::move(retained);
  }
  void validateStored(bool predecessor, const CancelToken& token) const {
    const auto& parent = predecessor ? localPreviousParent : localItemParent;
    const auto& proof = predecessor ? localPrevious : localTree;
    if (!parent || directoryProof(parent->location, token).attributes != parent->attributes) {
      throwFileError(FileErrorKind::Changed, "A local copy history containing folder changed.");
    }
    const auto names = ph::children(parent->location, token, 1);
    if (!proof) {
      if (!names.empty()) {
        throwFileError(FileErrorKind::Changed, "Unverified local history data was preserved.");
      }
      return;
    }
    if (names != std::vector<std::string>{payloadName}) {
      throwFileError(FileErrorKind::Changed, "Local copy history contains unknown entries.");
    }
    ph::verify(payload(predecessor), *proof, token);
  }
  void validateLocal(const CancelToken& token) const {
    if (!localTree || (previous && !localPrevious)) {
      throwFileError(FileErrorKind::Changed, "Local copy history is incomplete.");
    }
    validateStored(false, token);
    if (previous) {
      validateStored(true, token);
    }
    if (!hidden) {
      ph::verify(destination, tree, token);
    } else if (previous) {
      ph::verify(destination, *previous, token);
    } else {
      vacant(destination, token);
    }
  }
  void reverseLocal(const CancelToken& token, TransferResult& result, bool& changed) {
    result.source = destination;
    if (!hidden || previous) {
      ph::remove(destination, hidden ? *previous : tree, token);
      changed = true;
      result.children.emplace_back(destination, TransferOutcome::Moved);
      result.children.back().destination = payload(hidden);
    }
    if (hidden || previous) {
      const auto source = payload(!hidden);
      vacant(destination, token);
      const auto copied = copyItem(source, *destination.parent(), token);
      changed = true;
      result.children.emplace_back(source, TransferOutcome::Copied);
      result.children.back().destination = copied;
      if (!transferLocationsEqual(copied, destination)) {
        throwFileError(FileErrorKind::Changed, "Copy history published at an unexpected name.");
      }
      ph::Budget budget;
      auto restored = ph::capture(destination, budget, token);
      if (!sameContents(restored, hidden ? *localTree : *localPrevious)) {
        throwFileError(FileErrorKind::Changed, "The restored provider data changed.");
      }
      (hidden ? tree : *previous) = std::move(restored);
    }
    hidden = !hidden;
    validate(token);
    result.destination = hidden && !previous ? payload(false) : destination;
    result.outcome = TransferOutcome::Moved;
  }
  void cleanupLocal(const CancelToken& token) const {
    // Check both versions before discarding either one.
    for (bool predecessor : {false, true}) {
      if (predecessor ? localPreviousCreated : localItemCreated) {
        validateStored(predecessor, token);
      }
    }
    for (bool predecessor : {false, true}) {
      if (!(predecessor ? localPreviousCreated : localItemCreated)) {
        continue;
      }
      const auto& proof = predecessor ? localPrevious : localTree;
      if (proof) {
        ph::remove(payload(predecessor), *proof, token);
      }
      ph::erase(anchor.child(predecessor ? "previous" : "item"), token);
    }
  }
  std::vector<std::string> storageNames() const {
    if (local) {
      std::vector<std::string> names{kMarker};
      if (localItemCreated) {
        names.emplace_back("item");
      }
      if (localPreviousCreated) {
        names.emplace_back("previous");
      }
      return names;
    }
    if (hidden) {
      return {kMarker, "item"};
    }
    return previousStored ? std::vector<std::string>{kMarker, "previous"} : std::vector<std::string>{kMarker};
  }
  void validateParents(const CancelToken& token) const {
    for (const auto& parent : parents) {
      if (directoryProof(parent.location, token).attributes != parent.attributes) {
        throwFileError(FileErrorKind::Changed, "A remote copy containing folder changed.");
      }
    }
  }
  void validate(const CancelToken& token) const {
    validateParents(token);
    marker(anchor, nonce, token);
    const auto names = ph::children(anchor, token, local ? 3 : 2);
    if (names != storageNames() || (previous && !previousStored)) {
      throwFileError(FileErrorKind::Changed, "Remote history contains unknown entries.");
    }
    if (local) {
      validateLocal(token);
      return;
    }
    ph::verify(current(), tree, token);
    if (previous) {
      ph::verify(hidden ? destination : anchor.child("previous"), *previous, token);
      vacant(anchor.child(hidden ? "previous" : "item"), token);
    } else {
      vacant(hidden ? destination : anchor.child("item"), token);
    }
  }
  void cleanup() const {
    if (!anchorCreated) {
      if (anchorAttempted) {
        CleanupDeadline deadline;
        if (!local) {
          validateParents(deadline.token);
        }
        if (ph::info(anchor, deadline.token, true)) {
          throwFileError(FileErrorKind::Other, "Remote history storage creation was not confirmed.");
        }
      }
      return;
    }
    if (retain) {
      throwFileError(FileErrorKind::Other, "Incomplete remote reversal retained data for inspection.");
    }
    CleanupDeadline deadline;
    if (!local) {
      validateParents(deadline.token);
    }
    if (!markerReady) {
      if (!ph::children(anchor, deadline.token, 1).empty()) {
        throwFileError(FileErrorKind::Changed, "Unverified data remains in remote history storage.");
      }
    } else {
      marker(anchor, nonce, deadline.token);
      if (ph::children(anchor, deadline.token, local ? 3 : 2) != storageNames()) {
        throwFileError(FileErrorKind::Changed, "Unrecorded remote history data was preserved.");
      }
      if (local) {
        cleanupLocal(deadline.token);
      } else if (hidden || previousStored) {
        const auto location = anchor.child(hidden ? "item" : "previous");
        const auto& proof = hidden ? tree : *previous;
        ph::verify(location, proof, deadline.token);
        ph::remove(location, proof, deadline.token);
      }
      marker(anchor, nonce, deadline.token);
      ph::erase(anchor.child(kMarker), deadline.token);
    }
    ph::erase(anchor, deadline.token);
  }
};

std::shared_ptr<const luciole::ProviderCopyEffect> luciole::ProviderCopyEffect::prepareWithStorage(
    const Location& destination, const Location& staged, provider_history::Budget& budget, const CancelToken& token,
    bool replacement, const std::optional<Location>& historyRoot, bool local
) {
  auto parents = containingFolders(destination, historyRoot, token);
  auto tree = provider_history::capture(staged, budget, token);
  auto previous = replacement ? std::optional(provider_history::capture(destination, budget, token)) : std::nullopt;
  const auto parent = destination.parent();
  const auto name = destination.basename();
  if (!parent || !name) {
    throwFileError(FileErrorKind::Unsupported, "Remote copy history requires a containing folder.");
  }
  const GString uuid(g_uuid_string_random()), nonce(g_uuid_string_random());
  const auto storageParent = local ? Location::fromPath(g_get_tmp_dir()) : *parent;
  auto storage = std::shared_ptr<ProviderCopyStorage>(
      new ProviderCopyStorage(
          destination, storageParent.child(std::string(".luciole-history-") + uuid.get()), nonce.get(), std::move(tree),
          std::move(previous), std::move(parents), *name
      ),
      [](ProviderCopyStorage* raw) {
        auto owned = std::unique_ptr<ProviderCopyStorage>(raw);
        ++pendingCleanup;
        detail::runInThread(
            [owned = std::move(owned)] {
              try {
                owned->cleanup();
              } catch (...) {
                auto error = currentFileError();
                error.message += " Inspect remote history at " + owned->anchor.uri();
                std::println(stderr, "[file-history] {}", error.message);
                const std::scoped_lock lock(cleanupMutex);
                if (cleanupErrors.size() == 12) {
                  cleanupErrors.erase(cleanupErrors.begin());
                }
                cleanupErrors.push_back(std::move(error));
              }
            },
            [] { --pendingCleanup; }
        );
      }
  );
  storage->local = local;
  storage->anchorAttempted = true;
  createDirectory(storage->anchor, token);
  storage->anchorCreated = true;
  restrictDirectory(storage->anchor, token);
  writeMarker(storage->anchor.child(kMarker), storage->nonce, token);
  storage->markerReady = true;
  if (local) {
    storage->storeLocal(staged, storage->tree, false, token);
    if (storage->previous) {
      storage->storeLocal(destination, *storage->previous, true, token);
      storage->previousStored = true;
    }
  }
  return std::shared_ptr<const ProviderCopyEffect>(new ProviderCopyEffect(std::move(storage)));
}

std::shared_ptr<const luciole::ProviderCopyEffect> luciole::ProviderCopyEffect::prepare(
    const Location& destination, const Location& staged, provider_history::Budget& budget, const CancelToken& token,
    bool replacement, const std::optional<Location>& historyRoot
) {
  const bool local = destination.uri().starts_with("smb:")
      || destination.uri().starts_with("dav:")
      || destination.uri().starts_with("davs:");
  if (local) {
    return prepareLocal(destination, staged, budget, token, replacement, historyRoot);
  }
  return prepareWithStorage(destination, staged, budget, token, replacement, historyRoot, false);
}

std::shared_ptr<const luciole::ProviderCopyEffect> luciole::ProviderCopyEffect::prepareLocal(
    const Location& destination, const Location& staged, provider_history::Budget& budget, const CancelToken& token,
    bool replacement, const std::optional<Location>& historyRoot
) {
  return prepareWithStorage(destination, staged, budget, token, replacement, historyRoot, true);
}

void luciole::ProviderCopyEffect::published(const CancelToken& token, const std::optional<Location>& previous) const {
  if (m_storage->previous) {
    if (!previous) {
      throwFileError(FileErrorKind::Changed, "The overwritten destination was not retained.");
    }
    ph::verify(*previous, *m_storage->previous, token);
    if (!m_storage->local) {
      // An accepted rename can lose its reply. Preserve the anchor after any unconfirmed retention attempt.
      m_storage->retain = true;
      ph::move(*previous, m_storage->anchor.child("previous"), token);
      m_storage->previousStored = true;
    }
  }
  m_storage->validate(token);
  m_storage->published = true;
  m_storage->retain = false;
}

void luciole::ProviderCopyEffect::preserveInactive() const {
  m_storage->retain |= m_storage->hidden || m_storage->previousStored;
}

struct luciole::ProviderCopyReceipt::State {
  std::mutex mutex;
  std::vector<std::shared_ptr<const ProviderCopyEffect>> effects;
  std::uint64_t generation = 0;
};
luciole::ProviderCopyReceipt::ProviderCopyReceipt(std::shared_ptr<State> state, std::uint64_t generation)
    : m_state(std::move(state)), m_generation(generation) {}

std::shared_ptr<const luciole::CopyReceipt> luciole::ProviderCopyReceipt::capture(
    const std::vector<std::shared_ptr<const ProviderCopyEffect>>& effects, const CancelToken& token
) {
  if (effects.empty() || effects.size() > 128) {
    return {};
  }
  auto state = std::make_shared<State>();
  std::vector<Location> destinations;
  destinations.reserve(effects.size());
  try {
    for (const auto& effect : effects) {
      const auto& destination = effect->m_storage->destination;
      if (!effect->m_storage->published || std::ranges::any_of(destinations, [&](const Location& earlier) {
            return transferLocationsEqual(destination, earlier)
                || transferLocationInside(destination, earlier)
                || transferLocationInside(earlier, destination);
          })) {
        throwFileError(FileErrorKind::Changed, "The remote copy selection cannot supply history.");
      }
      effect->m_storage->validate(token);
      destinations.push_back(destination);
    }
  } catch (...) {
    for (const auto& effect : effects) {
      effect->m_storage->retain |= effect->m_storage->hidden || effect->m_storage->previousStored;
    }
    throw;
  }
  state->effects = effects;
  return std::shared_ptr<const CopyReceipt>(new ProviderCopyReceipt(std::move(state), 0));
}

std::size_t luciole::ProviderCopyReceipt::size() const { return m_state->effects.size(); }

std::vector<luciole::TransferResult> luciole::ProviderCopyReceipt::failureResults(const FileError& error) const {
  std::vector<TransferResult> results;
  for (const auto& effect : m_state->effects) {
    auto value = error;
    value.message += " Inspect " + effect->m_storage->destination.uri() + " and " + effect->m_storage->anchor.uri();
    results.emplace_back(effect->m_storage->destination, TransferOutcome::Failed, std::move(value));
  }
  return results;
}

std::vector<luciole::TransferResult> luciole::ProviderCopyReceipt::unconfirmed(const FileError& error) const {
  const std::unique_lock lock(m_state->mutex, std::try_to_lock);
  if (lock.owns_lock() && m_generation == m_state->generation) {
    for (const auto& effect : m_state->effects) {
      effect->m_storage->retain |=
          effect->m_storage->local || effect->m_storage->hidden || effect->m_storage->previousStored;
    }
  }
  return failureResults(error);
}

luciole::CopyReplayResult
luciole::ProviderCopyReceipt::reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const {
  CopyReplayResult reply;
  for (const auto& effect : m_state->effects) {
    reply.results.emplace_back(effect->m_storage->destination, TransferOutcome::Skipped);
  }
  const std::unique_lock lock(m_state->mutex, std::try_to_lock);
  try {
    if (!lock.owns_lock()) {
      throwFileError(FileErrorKind::Other, "Another remote reversal is using this history.");
    }
    if (m_generation != m_state->generation) {
      throwFileError(FileErrorKind::Changed, "Remote copy history is stale.");
    }
    for (const auto& effect : m_state->effects) {
      effect->m_storage->validate(token);
    }
  } catch (...) {
    const auto error = currentFileError();
    reply.results = failureResults(error);
    if (error.kind == FileErrorKind::Cancelled) {
      for (auto& result : reply.results) {
        result.outcome = TransferOutcome::Cancelled;
      }
    }
    reply.retireHistory = invalidates(error);
    if (reply.retireHistory && lock.owns_lock() && m_generation == m_state->generation) {
      for (const auto& effect : m_state->effects) {
        effect->m_storage->retain |= effect->m_storage->hidden || effect->m_storage->previousStored;
      }
    }
    completed.store(size());
    return reply;
  }
  bool stopped = false, applied = false;
  for (std::size_t i = 0; i < size(); ++i) {
    auto& item = *m_state->effects[i]->m_storage;
    auto& result = reply.results[i];
    result.source = item.previous ? item.destination : item.current();
    if (stopped) {
      result.error = FileError{FileErrorKind::Other, "Not attempted after remote reversal stopped."};
      completed.fetch_add(1);
      continue;
    }
    bool attempted = false, moved = false;
    const auto from = item.previous ? item.destination : item.current();
    const auto to = item.previous ? item.anchor.child(item.hidden ? "previous" : "item")
        : item.hidden             ? item.destination
                                  : item.anchor.child("item");
    try {
      item.validate(token);
      provider_history::check(token);
      attempted = true;
      if (item.local) {
        item.reverseLocal(token, result, moved);
        applied |= moved;
      } else {
        provider_history::move(from, to, token);
        moved = applied = true;
        result.destination = to;
        if (item.previous) {
          result.children.emplace_back(from, TransferOutcome::Moved);
          result.children.back().destination = to;
          const auto restore = item.anchor.child(item.hidden ? "item" : "previous");
          ph::verify(to, item.hidden ? *item.previous : item.tree, token);
          ph::verify(restore, item.hidden ? item.tree : *item.previous, token);
          vacant(item.destination, token);
          ph::move(restore, item.destination, token);
          result.children.emplace_back(restore, TransferOutcome::Moved);
          result.children.back().destination = item.destination;
          result.destination = item.destination;
        }
        item.hidden = !item.hidden;
        item.validate(token);
        result.outcome = TransferOutcome::Moved;
      }
    } catch (...) {
      applied |= moved;
      auto error = currentFileError();
      bool uncertain = attempted && !moved;
      if (uncertain) {
        // A failed provider reply can follow a committed mutation. Do not assume zero effects.
        try {
          CleanupDeadline deadline;
          // Prove the entire old state, including both versions for replacement replay.
          item.validate(deadline.token);
          uncertain = false;
        } catch (...) {
        }
      }
      reply.retireHistory = applied || uncertain || invalidates(error);
      if (reply.retireHistory) {
        ++m_state->generation;
        for (const auto& effect : m_state->effects) {
          effect->m_storage->retain |= effect->m_storage->hidden || effect->m_storage->previousStored || uncertain;
        }
      }
      error.message += " Inspect " + item.destination.uri() + " and " + item.anchor.uri();
      result.error = std::move(error);
      result.outcome = moved || uncertain ? TransferOutcome::Partial
          : token.cancelled()             ? TransferOutcome::Cancelled
                                          : TransferOutcome::Failed;
      stopped = true;
    }
    completed.fetch_add(1);
  }
  if (!stopped) {
    ++m_state->generation;
    reply.receipt = std::shared_ptr<const CopyReceipt>(new ProviderCopyReceipt(m_state, m_state->generation));
  }
  return reply;
}

bool luciole::ProviderCopyReceipt::cleanupPending() { return pendingCleanup.load() != 0; }
std::vector<luciole::FileError> luciole::ProviderCopyReceipt::takeCleanupErrors() {
  const std::scoped_lock lock(cleanupMutex);
  return std::exchange(cleanupErrors, {});
}
