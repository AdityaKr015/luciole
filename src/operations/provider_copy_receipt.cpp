#include "operations/provider_copy_receipt.h"

#include "fs/background_job.h"

#include <algorithm>
#include <mutex>
#include <print>
#include <set>
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
  void privateDirectory(const Location& location, const CancelToken& token) {
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
  bool invalidates(const FileError& error) {
    return error.kind == FileErrorKind::Changed
        || error.kind == FileErrorKind::Missing
        || error.kind == FileErrorKind::NotDirectory
        || error.kind == FileErrorKind::Unsupported;
  }
} // namespace

struct luciole::ProviderCopyStorage {
  Location destination, anchor;
  std::string nonce;
  provider_history::Tree tree;
  std::optional<provider_history::Tree> previous;
  bool previousStored = false;
  bool anchorAttempted = false, anchorCreated = false, markerReady = false, published = false, hidden = false,
       retain = false;
  ProviderCopyStorage(
      Location target, Location storage, std::string secret, provider_history::Tree proof,
      std::optional<provider_history::Tree> predecessor
  )
      : destination(std::move(target)), anchor(std::move(storage)), nonce(std::move(secret)), tree(std::move(proof)),
        previous(std::move(predecessor)) {}

  Location current() const { return hidden ? anchor.child("item") : destination; }
  std::vector<std::string> storageNames() const {
    if (hidden) {
      return {kMarker, "item"};
    }
    return previousStored ? std::vector<std::string>{kMarker, "previous"} : std::vector<std::string>{kMarker};
  }
  void validate(const CancelToken& token) const {
    marker(anchor, nonce, token);
    const auto names = ph::children(anchor, token, 2);
    if (names != storageNames() || (previous && !previousStored)) {
      throwFileError(FileErrorKind::Changed, "Remote history contains unknown entries.");
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
    if (!markerReady) {
      if (!ph::children(anchor, deadline.token, 1).empty()) {
        throwFileError(FileErrorKind::Changed, "Unverified data remains in remote history storage.");
      }
    } else {
      marker(anchor, nonce, deadline.token);
      if (ph::children(anchor, deadline.token, 2) != storageNames()) {
        throwFileError(FileErrorKind::Changed, "Unrecorded remote history data was preserved.");
      }
      if (hidden || previousStored) {
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

std::shared_ptr<const luciole::ProviderCopyEffect> luciole::ProviderCopyEffect::prepare(
    const Location& destination, const Location& staged, provider_history::Budget& budget, const CancelToken& token,
    bool replacement
) {
  auto tree = provider_history::capture(staged, budget, token);
  auto previous = replacement ? std::optional(provider_history::capture(destination, budget, token)) : std::nullopt;
  const auto parent = destination.parent();
  if (!parent) {
    throwFileError(FileErrorKind::Unsupported, "Remote copy history requires a containing folder.");
  }
  const GString uuid(g_uuid_string_random()), nonce(g_uuid_string_random());
  auto storage = std::shared_ptr<ProviderCopyStorage>(
      new ProviderCopyStorage(
          destination, parent->child(std::string(".luciole-history-") + uuid.get()), nonce.get(), std::move(tree),
          std::move(previous)
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
  storage->anchorAttempted = true;
  privateDirectory(storage->anchor, token);
  storage->anchorCreated = true;
  restrictDirectory(storage->anchor, token);
  writeMarker(storage->anchor.child(kMarker), storage->nonce, token);
  storage->markerReady = true;
  return std::shared_ptr<const ProviderCopyEffect>(new ProviderCopyEffect(std::move(storage)));
}

void luciole::ProviderCopyEffect::published(const CancelToken& token, const std::optional<Location>& previous) const {
  if (m_storage->previous) {
    if (!previous) {
      throwFileError(FileErrorKind::Changed, "The overwritten destination was not retained.");
    }
    ph::verify(*previous, *m_storage->previous, token);
    // An accepted rename can lose its reply. Preserve the anchor after any unconfirmed retention attempt.
    m_storage->retain = true;
    ph::move(*previous, m_storage->anchor.child("previous"), token);
    m_storage->previousStored = true;
  }
  m_storage->validate(token);
  m_storage->published = true;
  m_storage->retain = false;
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
  std::set<std::string> destinations;
  try {
    for (const auto& effect : effects) {
      if (!effect->m_storage->published || !destinations.insert(effect->m_storage->destination.uri()).second) {
        throwFileError(FileErrorKind::Changed, "The remote copy selection cannot supply history.");
      }
      effect->m_storage->validate(token);
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
      effect->m_storage->retain |= effect->m_storage->hidden || effect->m_storage->previousStored;
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
    } catch (...) {
      auto error = currentFileError();
      bool uncertain = attempted && !moved;
      if (uncertain) {
        // A failed provider reply can follow an accepted rename. Do not assume zero effects.
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
      error.message += " Inspect " + from.uri() + " and " + item.anchor.uri();
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
