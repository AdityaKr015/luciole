#include "operations/trash_service.h"

#include "fs/background_job.h"
#include "fs/gio_interop.h"
#include "fs/posix_fd.h"

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
  struct Target {
    luciole::Location location;
    std::string name;
    luciole::Fd item;
    Target(luciole::Location location, std::string name, int parent)
        : location(std::move(location)), name(std::move(name)),
          item(luciole::checkedFd(::openat(parent, this->name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC))) {}
  };
  struct Prepared {
    std::string path;
    luciole::Fd parent;
    std::vector<std::unique_ptr<Target>> targets;
    explicit Prepared(std::string path)
        : path(std::move(path)),
          parent(luciole::checkedFd(::open(this->path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC))) {}
  };
  struct Reply {
    std::shared_ptr<Prepared> prepared;
    std::vector<luciole::TrashResult> results;
    bool finished = false;
  };
} // namespace

struct luciole::TrashService::State {
  TrashSnapshot snapshot;
  std::shared_ptr<Prepared> prepared;
  CancelToken token;
  std::atomic<std::size_t> completed{0};
  bool active = false;
  std::function<void()> changed;
  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  static void launch(const std::shared_ptr<State>& owner, std::move_only_function<Reply(const CancelToken&)> work) {
    owner->active = true;
    owner->token = CancelToken{};
    runJob<Reply>(
        owner->token, [token = owner->token, work = std::move(work)] mutable { return work(token); },
        [owner, token = owner->token](JobResult<Reply> result) {
          owner->active = false;
          owner->snapshot.error.reset();
          if (!result && result.error().kind != FileErrorKind::Cancelled) {
            owner->snapshot.error = std::move(result.error());
          }
          if (result && result->finished) {
            owner->snapshot.results = std::move(result->results);
            owner->snapshot.status = TrashStatus::Finished;
            owner->prepared.reset();
          } else if (token.cancelled()) {
            owner->snapshot.status = TrashStatus::Cancelled;
            owner->prepared.reset();
          } else if (!result) {
            owner->snapshot.status = TrashStatus::Error;
            owner->prepared.reset();
          } else {
            owner->prepared = std::move(result->prepared);
            owner->snapshot.status = TrashStatus::Ready;
          }
          owner->notify();
        }
    );
    owner->notify();
  }
};

luciole::TrashService::TrashService() : m_state(std::make_shared<State>()) {}
luciole::TrashService::~TrashService() {
  m_state->changed = {};
  cancel();
}
void luciole::TrashService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
bool luciole::TrashService::busy() const { return m_state->active; }
bool luciole::TrashService::canTrash() const { return !busy() && m_state->prepared != nullptr; }
std::size_t luciole::TrashService::completed() const { return m_state->completed.load(); }
const luciole::TrashSnapshot& luciole::TrashService::snapshot() const { return m_state->snapshot; }
void luciole::TrashService::cancel() {
  m_state->token.cancel();
  m_state->prepared.reset();
  if (!busy() && m_state->snapshot.status != TrashStatus::Finished) {
    m_state->snapshot.status = TrashStatus::Cancelled;
  }
}
bool luciole::TrashService::prepare(const Location& parent, std::vector<Location> sources) {
  if (busy() || sources.empty()) {
    return false;
  }
  m_state->prepared.reset();
  m_state->completed.store(0);
  m_state->snapshot = {.status = TrashStatus::Preparing, .results = {}, .error = {}};
  for (const auto& source : sources) {
    m_state->snapshot.results.push_back({source, TrashOutcome::Cancelled, {}});
  }
  State::launch(m_state, [parent, sources = std::move(sources)](const CancelToken& token) {
    const auto path = parent.nativePath();
    if (!path) {
      throwFileError(FileErrorKind::Unsupported, "Moving remote items to Trash is not supported yet.");
    }
    auto prepared = std::make_shared<Prepared>(*path);
    std::set<std::string> names;
    for (const auto& source : sources) {
      if (token.cancelled()) {
        return Reply{};
      }
      const auto native = source.nativePath();
      if (!native || std::filesystem::path(*native).parent_path().native() != *path) {
        throwFileError(FileErrorKind::InvalidArgument, "Trash requires items from the current native folder.");
      }
      const auto name = std::filesystem::path(*native).filename().native();
      if (name.empty() || name == "." || name == ".." || !names.insert(name).second) {
        throwFileError(FileErrorKind::InvalidArgument, "The Trash selection contains an invalid or repeated item.");
      }
      prepared->targets.push_back(std::make_unique<Target>(source, name, prepared->parent.get()));
    }
    return Reply{.prepared = std::move(prepared), .results = {}, .finished = false};
  });
  return true;
}
bool luciole::TrashService::trash() {
  if (!canTrash()) {
    return false;
  }
  m_state->snapshot.status = TrashStatus::Trashing;
  State::launch(m_state, [owner = m_state, prepared = m_state->prepared](const CancelToken& token) {
    Reply reply{.prepared = {}, .results = {}, .finished = true};
    for (const auto& target : prepared->targets) {
      reply.results.push_back({target->location, TrashOutcome::Cancelled, {}});
    }
    for (std::size_t index = 0; index < prepared->targets.size(); ++index) {
      if (token.cancelled()) {
        break;
      }
      const auto& target = *prepared->targets[index];
      auto& result = reply.results[index];
      try {
        const auto currentParent = checkedFd(::open(prepared->path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!sameIdentity(statFd(prepared->parent.get()), statFd(currentParent.get()))) {
          throwFileError(FileErrorKind::Changed, "The containing folder changed. Select the items again.");
        }
        struct stat current{};
        if (::fstatat(prepared->parent.get(), target.name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0) {
          throwErrno(errno);
        }
        if (!sameIdentity(statFd(target.item.get()), current)) {
          throwFileError(FileErrorKind::Changed, "The item was replaced or moved. Select it again.");
        }
        if (token.cancelled()) {
          break;
        }
        GError* rawError = nullptr;
        if (g_file_trash(gioFile(target.location).get(), gioCancellable(token), &rawError)) {
          result.outcome = TrashOutcome::Trashed;
        } else {
          const GErrorPtr error(rawError);
          result.error = error
              ? fileError(*error)
              : FileError{.kind = FileErrorKind::Other, .message = "Could not move the item to Trash."};
          result.outcome =
              result.error->kind == FileErrorKind::Cancelled ? TrashOutcome::Cancelled : TrashOutcome::Failed;
        }
      } catch (const std::exception&) {
        result.outcome = TrashOutcome::Failed;
        result.error = currentFileError();
      }
      owner->completed.fetch_add(1);
    }
    return reply;
  });
  return true;
}
