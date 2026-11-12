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
  bool anchorAttempted = false, anchorCreated = false, markerReady = false, published = false, hidden = false,
       retain = false;
  ProviderCopyStorage(Location target, Location storage, std::string secret, provider_history::Tree proof)
      : destination(std::move(target)), anchor(std::move(storage)), nonce(std::move(secret)), tree(std::move(proof)) {}

  Location current() const { return hidden ? anchor.child("item") : destination; }
  void validate(const CancelToken& token) const {
    marker(anchor, nonce, token);
    const auto names = ph::children(anchor, token, 2);
    const std::vector<std::string> expected =
        hidden ? std::vector<std::string>{kMarker, "item"} : std::vector<std::string>{kMarker};
    if (names != expected) {
      throwFileError(FileErrorKind::Changed, "Remote history contains unknown entries.");
    }
    ph::verify(current(), tree, token);
    vacant(hidden ? destination : anchor.child("item"), token);
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
      const auto expected = hidden ? std::vector<std::string>{kMarker, "item"} : std::vector<std::string>{kMarker};
      if (ph::children(anchor, deadline.token, 2) != expected) {
        throwFileError(FileErrorKind::Changed, "Unrecorded remote history data was preserved.");
      }
      if (hidden) {
        ph::verify(anchor.child("item"), tree, deadline.token);
        ph::remove(anchor.child("item"), tree, deadline.token);
      }
      marker(anchor, nonce, deadline.token);
      ph::erase(anchor.child(kMarker), deadline.token);
    }
    ph::erase(anchor, deadline.token);
  }
};

std::shared_ptr<const luciole::ProviderCopyEffect> luciole::ProviderCopyEffect::prepare(
    const Location& destination, const Location& staged, provider_history::Budget& budget, const CancelToken& token
) {
  auto tree = provider_history::capture(staged, budget, token);
  const auto parent = destination.parent();
  if (!parent) {
    throwFileError(FileErrorKind::Unsupported, "Remote copy history requires a containing folder.");
  }
  const GString uuid(g_uuid_string_random()), nonce(g_uuid_string_random());
  auto storage = std::shared_ptr<ProviderCopyStorage>(
      new ProviderCopyStorage(
          destination, parent->child(std::string(".luciole-history-") + uuid.get()), nonce.get(), std::move(tree)
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

void luciole::ProviderCopyEffect::published(const CancelToken& token) const {
  m_storage->validate(token);
  m_storage->published = true;
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
  for (const auto& effect : effects) {
    if (!effect->m_storage->published || !destinations.insert(effect->m_storage->destination.uri()).second) {
      throwFileError(FileErrorKind::Changed, "The remote copy selection cannot supply history.");
    }
    effect->m_storage->validate(token);
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
      effect->m_storage->retain |= effect->m_storage->hidden;
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
        effect->m_storage->retain |= effect->m_storage->hidden;
      }
    }
    completed.store(size());
    return reply;
  }
  bool stopped = false, applied = false;
  for (std::size_t i = 0; i < size(); ++i) {
    auto& item = *m_state->effects[i]->m_storage;
    auto& result = reply.results[i];
    result.source = item.current();
    if (stopped) {
      result.error = FileError{FileErrorKind::Other, "Not attempted after remote reversal stopped."};
      completed.fetch_add(1);
      continue;
    }
    bool attempted = false, moved = false;
    const auto from = item.current(), to = item.hidden ? item.destination : item.anchor.child("item");
    try {
      item.validate(token);
      provider_history::check(token);
      attempted = true;
      provider_history::move(from, to, token);
      moved = applied = true;
      item.hidden = !item.hidden;
      result.destination = to;
      item.validate(token);
      result.outcome = TransferOutcome::Moved;
    } catch (...) {
      auto error = currentFileError();
      bool uncertain = attempted && !moved;
      if (uncertain) {
        // A failed provider reply can follow an accepted rename. Do not assume zero effects.
        try {
          CleanupDeadline deadline;
          provider_history::verify(from, item.tree, deadline.token);
          uncertain = false;
        } catch (...) {
        }
      }
      reply.retireHistory = applied || uncertain || invalidates(error);
      if (reply.retireHistory) {
        ++m_state->generation;
        for (const auto& effect : m_state->effects) {
          effect->m_storage->retain |= effect->m_storage->hidden || uncertain;
        }
      }
      error.message += " Inspect " + from.uri() + " and " + to.uri();
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
