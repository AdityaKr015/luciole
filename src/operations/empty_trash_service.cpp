#include "operations/empty_trash_service.h"

#include "fs/background_job.h"
#include "fs/gio_interop.h"
#include "operations/trash_item.h"

#include <atomic>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace {
  using Targets = std::vector<files::TrashItem>;
  struct Size {
    std::optional<std::uint64_t> bytes = 0;
    bool incomplete = false;
  };
  Size measureSize(const Targets& targets, const files::CancelToken& token) {
    struct Folder {
      files::Location location;
      unsigned int depth;
    };
    Size reply;
    std::vector<Folder> pending;
    constexpr const char* attributes = "standard::name,standard::type,standard::is-symlink,standard::size";
    auto* cancellable = files::gioCancellable(token);
    const auto inspect = [&](const files::Location& location, GFileInfo* info, unsigned int depth) {
      const auto type = g_file_info_get_file_type(info);
      if (type == G_FILE_TYPE_DIRECTORY && !g_file_info_get_is_symlink(info)) {
        if (depth >= 256 || pending.size() >= 65536) {
          reply.incomplete = true;
        } else {
          pending.push_back({location, depth});
        }
      } else if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_STANDARD_SIZE) == G_FILE_ATTRIBUTE_TYPE_UINT64) {
        const auto size = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_STANDARD_SIZE);
        if (size > std::numeric_limits<std::uint64_t>::max() - *reply.bytes) {
          reply.incomplete = true;
        } else {
          *reply.bytes += size;
        }
      } else {
        reply.incomplete = true;
      }
    };
    for (const auto& target : targets) {
      if (token.cancelled()) {
        reply.incomplete = true;
        return reply;
      }
      const files::GioPtr<GFileInfo> info(g_file_query_info(
          files::gioFile(target.location).get(), attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr
      ));
      if (info) {
        inspect(target.location, info.get(), 0);
      } else {
        reply.incomplete = true;
      }
    }
    while (!pending.empty()) {
      if (token.cancelled()) {
        reply.incomplete = true;
        return reply;
      }
      const auto folder = std::move(pending.back());
      pending.pop_back();
      const files::GioPtr<GFileEnumerator> enumerator(g_file_enumerate_children(
          files::gioFile(folder.location).get(), attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, nullptr
      ));
      if (!enumerator) {
        reply.incomplete = true;
        continue;
      }
      for (;;) {
        GError* raw = nullptr;
        const files::GioPtr<GFileInfo> info(g_file_enumerator_next_file(enumerator.get(), cancellable, &raw));
        const files::GErrorPtr error(raw);
        if (!info) {
          reply.incomplete = reply.incomplete || error != nullptr;
          break;
        }
        const char* name = g_file_info_get_name(info.get());
        if (name == nullptr
            || *name == '\0'
            || std::string_view(name).contains('/')
            || std::string_view(name) == "."
            || std::string_view(name) == "..") {
          reply.incomplete = true;
          continue;
        }
        inspect(folder.location.child(name), info.get(), folder.depth + 1);
      }
      if (!g_file_enumerator_close(enumerator.get(), cancellable, nullptr)) {
        reply.incomplete = true;
      }
    }
    return reply;
  }
  std::vector<files::EmptyTrashResult> pendingResults(const Targets& targets) {
    std::vector<files::EmptyTrashResult> results;
    for (const auto& target : targets) {
      results.push_back({target.location, target.displayName, files::EmptyTrashOutcome::Cancelled, {}});
    }
    return results;
  }
  files::EmptyTrashOutcome failedDelete(files::FileErrorKind kind) {
    using Kind = files::FileErrorKind;
    using Outcome = files::EmptyTrashOutcome;
    switch (kind) {
    case Kind::Missing:
      return Outcome::Skipped;
    case Kind::Denied:
    case Kind::Unsupported:
    case Kind::ReadOnly:
    case Kind::NotEmpty:
    case Kind::InvalidArgument:
      return Outcome::Failed;
    default:
      return Outcome::Unconfirmed;
    }
  }
} // namespace

struct files::EmptyTrashService::State {
  EmptyTrashSnapshot snapshot;
  std::shared_ptr<Targets> targets;
  CancelToken token;
  std::atomic<std::size_t> completed{0};
  bool active = false;
  bool emptyRequested = false;
  std::function<void()> changed;
  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  template <typename Reply> using Done = void (*)(const std::shared_ptr<State>&, const CancelToken&, JobResult<Reply>);
  // Runs work under the current token; done receives that same token.
  template <typename Reply>
  static void launch(const std::shared_ptr<State>& owner, std::move_only_function<Reply()> work, Done<Reply> done) {
    owner->active = true;
    const auto token = owner->token;
    runJob<Reply>(token, std::move(work), [owner, token, done](JobResult<Reply> result) {
      owner->active = false;
      done(owner, token, std::move(result));
    });
    owner->notify();
  }
  static void prepared(const std::shared_ptr<State>& owner, const CancelToken& token, JobResult<Targets> reply) {
    owner->snapshot.error = reply ? std::nullopt : std::optional(reply.error());
    if (token.cancelled()) {
      owner->snapshot.status = EmptyTrashStatus::Cancelled;
      owner->targets.reset();
    } else if (!reply) {
      owner->snapshot.status = EmptyTrashStatus::Error;
      owner->targets.reset();
    } else {
      owner->targets = std::make_shared<Targets>(std::move(*reply));
      owner->snapshot.results = pendingResults(*owner->targets);
      owner->snapshot.status = EmptyTrashStatus::Ready;
      launch<Size>(owner, [targets = owner->targets, token] { return measureSize(*targets, token); }, measured);
      return;
    }
    owner->notify();
  }
  static void measured(const std::shared_ptr<State>& owner, const CancelToken& token, JobResult<Size> reply) {
    if (reply) {
      owner->snapshot.size = reply->bytes;
      owner->snapshot.sizeIncomplete = reply->incomplete;
    } else {
      // A job skipped by cancellation reports nothing rather than an incomplete size.
      owner->snapshot.size = std::nullopt;
      owner->snapshot.sizeIncomplete = reply.error().kind != FileErrorKind::Cancelled;
    }
    if (std::exchange(owner->emptyRequested, false)) {
      owner->token = CancelToken{};
      startEmpty(owner);
      return;
    }
    if (token.cancelled()) {
      owner->snapshot.status = EmptyTrashStatus::Cancelled;
      owner->targets.reset();
    }
    owner->notify();
  }
  static void emptied(
      const std::shared_ptr<State>& owner, const CancelToken& token, JobResult<std::vector<EmptyTrashResult>> reply
  ) {
    owner->targets.reset();
    if (reply) {
      owner->snapshot.error.reset();
      owner->snapshot.results = std::move(*reply);
      owner->snapshot.status = EmptyTrashStatus::Finished;
    } else {
      owner->snapshot.error = reply.error();
      owner->snapshot.status = token.cancelled() ? EmptyTrashStatus::Cancelled : EmptyTrashStatus::Error;
    }
    owner->notify();
  }
  static void startEmpty(const std::shared_ptr<State>& owner);
};

files::EmptyTrashService::EmptyTrashService() : m_state(std::make_shared<State>()) {}
files::EmptyTrashService::~EmptyTrashService() {
  m_state->changed = {};
  cancel();
}
void files::EmptyTrashService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
bool files::EmptyTrashService::busy() const { return m_state->active; }
bool files::EmptyTrashService::pending() const { return busy() || m_state->snapshot.status == EmptyTrashStatus::Ready; }
bool files::EmptyTrashService::canEmpty() const {
  return m_state->snapshot.status == EmptyTrashStatus::Ready && m_state->targets && !m_state->targets->empty();
}
std::size_t files::EmptyTrashService::completed() const { return m_state->completed.load(); }
const files::EmptyTrashSnapshot& files::EmptyTrashService::snapshot() const { return m_state->snapshot; }
void files::EmptyTrashService::cancel() {
  m_state->emptyRequested = false;
  m_state->token.cancel();
  m_state->targets.reset();
  if (!busy() && m_state->snapshot.status != EmptyTrashStatus::Finished) {
    m_state->snapshot.status = EmptyTrashStatus::Cancelled;
  }
}
bool files::EmptyTrashService::prepare() { return prepareTargets(std::nullopt); }
bool files::EmptyTrashService::prepareSelection(std::vector<DirectoryEntry> selection) {
  if (selection.empty() || selection.size() > 4096) {
    return false;
  }
  return prepareTargets(std::move(selection));
}
bool files::EmptyTrashService::prepareTargets(std::optional<std::vector<DirectoryEntry>> selection) {
  if (busy()) {
    return false;
  }
  m_state->targets.reset();
  m_state->token = CancelToken{};
  m_state->completed.store(0);
  m_state->snapshot = {
      .status = EmptyTrashStatus::Preparing, .results = {}, .error = {}, .size = std::nullopt, .sizeIncomplete = false
  };
  State::launch<Targets>(
      m_state,
      [token = m_state->token, selection = std::move(selection)] {
        if (!selection) {
          return listTrashItems(token);
        }
        Targets targets;
        std::set<std::string> seen;
        for (const auto& entry : *selection) {
          if (!isTopLevelTrashItem(entry.location) || !seen.insert(entry.location.uri()).second) {
            throwFileError(FileErrorKind::InvalidArgument, "Only distinct top-level Trash items can be deleted.");
          }
          const auto current = queryTrashItem(entry.location, token);
          const auto expected = readTrashItemIdentity(entry);
          if (expected != current.identity()) {
            throwFileError(FileErrorKind::Changed, "The selected item changed. Select it again before deleting it.");
          }
          targets.push_back({entry.location, entry.displayName, expected});
        }
        return targets;
      },
      State::prepared
  );
  return true;
}
bool files::EmptyTrashService::empty() {
  if (!canEmpty()) {
    return false;
  }
  m_state->snapshot.status = EmptyTrashStatus::Emptying;
  if (busy()) {
    m_state->emptyRequested = true;
    m_state->token.cancel();
    m_state->notify();
  } else {
    m_state->token = CancelToken{};
    State::startEmpty(m_state);
  }
  return true;
}
void files::EmptyTrashService::State::startEmpty(const std::shared_ptr<State>& owner) {
  launch<std::vector<EmptyTrashResult>>(
      owner,
      [owner, token = owner->token, targets = owner->targets] {
        auto results = pendingResults(*targets);
        for (std::size_t index = 0; index < targets->size(); ++index) {
          if (token.cancelled()) {
            break;
          }
          const auto& target = (*targets)[index];
          auto& result = results[index];
          try {
            if (target.identity != queryTrashItem(target.location, token).identity()) {
              result.outcome = EmptyTrashOutcome::Failed;
              result.error = FileError{
                  .kind = FileErrorKind::Changed,
                  .message = "The item changed after confirmation was prepared. It was left in Trash."
              };
            } else {
              if (token.cancelled()) {
                break;
              }
              GError* raw = nullptr;
              // Finish the current provider request before observing cancellation.
              const bool removed = g_file_delete(gioFile(target.location).get(), nullptr, &raw);
              const GErrorPtr error(raw);
              if (removed) {
                result.outcome = EmptyTrashOutcome::Deleted;
              } else {
                result.error = error
                    ? fileError(*error)
                    : FileError{.kind = FileErrorKind::Other, .message = "Could not confirm deletion."};
                result.outcome = failedDelete(result.error->kind);
              }
            }
          } catch (const std::exception&) {
            auto error = currentFileError();
            if (error.kind == FileErrorKind::Cancelled) {
              break;
            }
            result.outcome =
                error.kind == FileErrorKind::Missing ? EmptyTrashOutcome::Skipped : EmptyTrashOutcome::Failed;
            result.error = std::move(error);
          }
          owner->completed.fetch_add(1);
        }
        return results;
      },
      emptied
  );
}
