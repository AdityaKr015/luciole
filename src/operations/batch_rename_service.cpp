#include "operations/batch_rename_service.h"

#include "fs/filename.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <linux/fs.h>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace {
  class Fd {
  public:
    explicit Fd(int fd) : m_fd(fd) {
      if (fd < 0) {
        throw std::runtime_error(std::strerror(errno));
      }
    }
    ~Fd() { ::close(m_fd); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return m_fd; }

  private:
    int m_fd;
  };
  struct stat metadata(int fd) {
    struct stat value{};
    if (::fstat(fd, &value) != 0) {
      throw std::runtime_error(std::strerror(errno));
    }
    return value;
  }
  bool same(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && (a.st_mode & S_IFMT) == (b.st_mode & S_IFMT);
  }
  std::string display(const std::string& name) {
    const files::GString text(g_filename_display_name(name.c_str()));
    return text.get();
  }
  std::string fdPath(int fd) {
    std::vector<char> buffer(8192);
    const auto count = ::readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), buffer.data(), buffer.size());
    if (count <= 0 || static_cast<std::size_t>(count) == buffer.size()) {
      throw std::runtime_error("The held folder's current path is unavailable.");
    }
    return {buffer.data(), static_cast<std::size_t>(count)};
  }
  struct Item {
    files::DirectoryEntry entry;
    std::string name;
    Fd fd;
    struct stat identity;
    Item(files::DirectoryEntry entry, int parent, std::string name)
        : entry(std::move(entry)), name(std::move(name)),
          fd(::openat(parent, this->name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC)), identity(metadata(fd.get())) {}
    bool at(int parent, const std::string& name) const {
      struct stat value{};
      return ::fstatat(parent, name.c_str(), &value, AT_SYMLINK_NOFOLLOW) == 0 && same(value, identity);
    }
  };
  struct Prepared {
    files::Location parent;
    std::string path;
    Fd fd;
    std::vector<std::unique_ptr<Item>> items;
    long nameLimit;
    explicit Prepared(files::Location parent)
        : parent(std::move(parent)), path(*this->parent.nativePath()),
          fd(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)), nameLimit(::fpathconf(fd.get(), _PC_NAME_MAX)) {
    }
    void requireCurrent() const {
      const Fd current(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
      if (!same(metadata(current.get()), metadata(fd.get()))) {
        throw std::runtime_error("The containing folder changed. Close this dialog and select the items again.");
      }
    }
    std::optional<files::Location>
    actual(int directory, const std::string& name, const Item& item, bool originalFolder) const {
      if (!item.at(directory, name)) {
        return std::nullopt;
      }
      try {
        std::string base;
        if (originalFolder) {
          try {
            requireCurrent();
            base = path;
          } catch (const std::exception&) {
            base = fdPath(directory);
          }
        } else {
          base = fdPath(directory);
        }
        return files::Location::fromPath((std::filesystem::path(base) / name).native());
      } catch (const std::exception&) {
        return std::nullopt;
      }
    }
  };
  std::shared_ptr<Prepared> prepareItems(std::vector<files::DirectoryEntry> entries, const std::atomic<bool>& stop) {
    const files::GioPtr<GFile> first(g_file_get_parent(entries.front().location.file().get()));
    if (!first) {
      throw std::runtime_error("Select entries inside a native folder.");
    }
    const files::GString uri(g_file_get_uri(first.get()));
    const auto parent = files::Location::fromUri(uri.get());
    if (!parent.nativePath()) {
      throw std::runtime_error("Batch rename requires a native folder.");
    }
    auto prepared = std::make_shared<Prepared>(parent);
    std::set<std::string> names;
    for (auto& entry : entries) {
      if (stop.load()) {
        return {};
      }
      const files::GioPtr<GFile> containing(g_file_get_parent(entry.location.file().get()));
      if (!entry.location.nativePath()
          || !entry.identity
          || !containing
          || !g_file_equal(first.get(), containing.get())) {
        throw std::runtime_error("Select identified items from one native folder.");
      }
      const files::GString raw(g_file_get_basename(entry.location.file().get()));
      const std::string name = raw ? raw.get() : "";
      if (name.empty() || name == "." || name == ".." || name != entry.name || !names.insert(name).second) {
        throw std::runtime_error("The selection contains invalid or repeated names.");
      }
      auto item = std::make_unique<Item>(entry, prepared->fd.get(), name);
      const files::GioPtr<GFileInfo> info(g_file_query_info(
          entry.location.file().get(), "id::file,id::filesystem,standard::type,standard::is-symlink,access::can-rename",
          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
      ));
      if (!info
          || files::readFileIdentity(info.get()) != entry.identity
          || g_file_info_get_file_type(info.get()) != entry.type
          || (g_file_info_get_is_symlink(info.get()) != FALSE) != entry.symlink
          || !item->at(prepared->fd.get(), name)
          || !files::readFileAccess(info.get()).rename.value_or(true)) {
        throw std::runtime_error("A selected item changed or cannot be renamed. Select it again.");
      }
      prepared->items.push_back(std::move(item));
    }
    prepared->requireCurrent();
    return prepared;
  }
  std::shared_ptr<std::vector<files::BatchRenameRow>>
  validate(const Prepared& plan, const std::vector<std::string>& names, const std::atomic<bool>& stop) {
    auto rows = std::make_shared<std::vector<files::BatchRenameRow>>();
    std::map<std::string, std::size_t> sources, targets;
    for (std::size_t i = 0; i < names.size(); ++i) {
      sources.emplace(plan.items[i]->name, i);
      rows->push_back(
          {plan.items[i]->entry.displayName, display(names[i]), names[i], {}, names[i] != plan.items[i]->name}
      );
    }
    plan.requireCurrent();
    const bool writable = ::faccessat(plan.fd.get(), ".", W_OK | X_OK, AT_EACCESS) == 0;
    for (std::size_t i = 0; i < names.size() && !stop.load(); ++i) {
      auto& row = (*rows)[i];
      if (!plan.items[i]->at(plan.fd.get(), plan.items[i]->name)) {
        row.error = "Source changed. Close and select it again.";
      }
      if (row.changed) {
        if (const auto problem = files::filenameProblem(row.name)) {
          row.error = *problem;
        }
        if (plan.nameLimit >= 0 && row.name.size() > static_cast<std::size_t>(plan.nameLimit)) {
          row.error = "Name is too long for this folder.";
        }
        if (!writable) {
          row.error = "The containing folder is not writable.";
        }
      }
      const auto [found, inserted] = targets.emplace(row.name, i);
      if (!inserted) {
        row.error = "Duplicate destination name.";
        (*rows)[found->second].error = row.error;
      }
      if (row.changed && row.error.empty() && !sources.contains(row.name)) {
        struct stat info{};
        if (::fstatat(plan.fd.get(), row.name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
          row.error = "An unselected item already has this name.";
        } else if (errno != ENOENT) {
          row.error = std::strerror(errno);
        }
      }
    }
    return rows;
  }
  bool applicable(const std::vector<files::BatchRenameRow>& rows) {
    return std::ranges::any_of(rows, [](const auto& row) { return row.changed; })
        && std::ranges::none_of(rows, [](const auto& row) { return !row.error.empty(); });
  }
  class Stage {
  public:
    explicit Stage(const Prepared& plan) : m_parent(plan.fd.get()) {
      const files::GString uuid(g_uuid_string_random());
      m_name = ".noctalia-rename-" + std::string(uuid.get());
      if (::mkdirat(m_parent, m_name.c_str(), 0700) != 0) {
        throw std::runtime_error(std::strerror(errno));
      }
      const int fd = ::openat(m_parent, m_name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0) {
        const int code = errno;
        (void)::unlinkat(m_parent, m_name.c_str(), AT_REMOVEDIR);
        throw std::runtime_error(std::strerror(code));
      }
      m_fd = std::make_unique<Fd>(fd);
    }
    ~Stage() {
      struct stat current{}, held{};
      if (m_fd
          && ::fstat(m_fd->get(), &held) == 0
          && ::fstatat(m_parent, m_name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0
          && same(current, held)) {
        // Never remove contents: incomplete recovery must remain available for inspection.
        (void)::unlinkat(m_parent, m_name.c_str(), AT_REMOVEDIR);
      }
    }
    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;
    int fd() const { return m_fd->get(); }

  private:
    int m_parent;
    std::string m_name;
    std::unique_ptr<Fd> m_fd;
  };
  void move(const Item& item, int from, const std::string& oldName, int to, const std::string& newName) {
    if (!item.at(from, oldName)) {
      throw std::runtime_error("An item changed during the operation.");
    }
    if (::syscall(SYS_renameat2, from, oldName.c_str(), to, newName.c_str(), RENAME_NOREPLACE) != 0) {
      throw std::runtime_error(std::strerror(errno));
    }
  }
  std::vector<std::vector<std::size_t>>
  components(const Prepared& plan, const std::vector<files::BatchRenameRow>& rows) {
    std::map<std::string, std::size_t> old;
    std::vector<std::vector<std::size_t>> edges(rows.size()), groups;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      old.emplace(plan.items[i]->name, i);
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (const auto found = old.find(rows[i].name); rows[i].changed && found != old.end() && found->second != i) {
        edges[i].push_back(found->second);
        edges[found->second].push_back(i);
      }
    }
    std::vector<bool> seen(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (seen[i] || !rows[i].changed) {
        continue;
      }
      std::vector<std::size_t> group, pending{i};
      seen[i] = true;
      while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        group.push_back(index);
        for (const auto adjacent : edges[index]) {
          if (!seen[adjacent]) {
            seen[adjacent] = true;
            pending.push_back(adjacent);
          }
        }
      }
      std::ranges::sort(group);
      groups.push_back(std::move(group));
    }
    return groups;
  }
  enum class Position { Original, Staged, Published, Unknown };
  std::shared_ptr<std::vector<files::BatchRenameResult>> execute(
      const Prepared& plan, const std::vector<files::BatchRenameRow>& rows, const std::atomic<bool>& stop,
      std::atomic<std::size_t>& completed
  ) {
    using O = files::BatchRenameOutcome;
    auto results = std::make_shared<std::vector<files::BatchRenameResult>>();
    std::vector<Position> positions(rows.size(), Position::Original);
    for (std::size_t i = 0; i < rows.size(); ++i) {
      results->push_back(
          {plan.items[i]->entry.location, std::nullopt, rows[i].changed ? O::Cancelled : O::Unchanged, {}}
      );
      if (!rows[i].changed) {
        completed.fetch_add(1);
      }
    }
    std::unique_ptr<Stage> stage;
    for (const auto& group : components(plan, rows)) {
      if (stop.load()) {
        break;
      }
      std::string problem;
      try {
        plan.requireCurrent();
        if (!stage) {
          stage = std::make_unique<Stage>(plan);
        }
        for (const auto index : group) {
          if (stop.load()) {
            break;
          }
          plan.requireCurrent();
          move(*plan.items[index], plan.fd.get(), plan.items[index]->name, stage->fd(), std::to_string(index));
          positions[index] = Position::Staged;
        }
        if (!stop.load()) {
          // Once publication starts, finish this connected group before observing cancellation.
          for (const auto index : group) {
            plan.requireCurrent();
            move(*plan.items[index], stage->fd(), std::to_string(index), plan.fd.get(), rows[index].name);
            positions[index] = Position::Published;
          }
          for (const auto index : group) {
            (*results)[index].outcome = O::Renamed;
            completed.fetch_add(1);
          }
          continue;
        }
      } catch (const std::exception& error) {
        problem = error.what();
      }
      // Recover an incomplete connected group without replacing any intervening entry.
      for (const auto index : std::views::reverse(group)) {
        if (positions[index] != Position::Published) {
          continue;
        }
        try {
          move(*plan.items[index], plan.fd.get(), rows[index].name, stage->fd(), std::to_string(index));
          positions[index] = Position::Staged;
        } catch (const std::exception&) {
          positions[index] = Position::Unknown;
        }
      }
      for (const auto index : group) {
        auto& result = (*results)[index];
        result.outcome = problem.empty() ? O::Cancelled : O::Failed;
        result.error = problem;
        if (positions[index] == Position::Staged) {
          try {
            move(*plan.items[index], stage->fd(), std::to_string(index), plan.fd.get(), plan.items[index]->name);
            positions[index] = Position::Original;
          } catch (const std::exception& error) {
            result.outcome = O::NeedsAttention;
            result.error += std::string(" Original name could not be restored: ") + error.what();
          }
        } else if (positions[index] == Position::Unknown) {
          result.outcome = O::Unconfirmed;
          result.error += " The item's final location could not be confirmed.";
        }
        completed.fetch_add(1);
      }
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
      auto& result = (*results)[i];
      const bool staged = positions[i] == Position::Staged;
      if (positions[i] != Position::Unknown) {
        result.actual = plan.actual(
            staged ? stage->fd() : plan.fd.get(),
            staged                                    ? std::to_string(i)
                : positions[i] == Position::Published ? rows[i].name
                                                      : plan.items[i]->name,
            *plan.items[i], !staged
        );
      }
      if (staged && result.actual) {
        result.error += " Retained at " + display(*result.actual->nativePath());
      }
      if (!result.actual && result.outcome != O::Unconfirmed) {
        result.outcome = O::Unconfirmed;
        result.error += " The item's final location could not be confirmed.";
      }
    }
    return results;
  }
} // namespace

std::string
files::batchRenameName(const std::string& name, bool directory, std::size_t index, const BatchRenameOptions& o) {
  if (o.pattern.size() > 4096
      || o.find.size() > 4096
      || o.replacement.size() > 4096
      || o.start < 0
      || o.start > 1000000000
      || o.digits < 1
      || o.digits > 9) {
    throw std::runtime_error("Rename options exceed their limits.");
  }
  const auto stemLength = o.keepExtensions ? filenameStemLength(name, directory) : name.size();
  const auto stem = name.substr(0, stemLength), extension = name.substr(stemLength);
  std::string result;
  const auto append = [&](std::string_view text) {
    if (result.size() + text.size() > 4096) {
      throw std::runtime_error("Generated name is too long.");
    }
    result += text;
  };
  if (o.mode == BatchRenameMode::Numbering) {
    auto number = std::to_string(static_cast<std::uint64_t>(o.start) + index);
    number.insert(
        0, number.size() < static_cast<std::size_t>(o.digits) ? static_cast<std::size_t>(o.digits) - number.size() : 0,
        '0'
    );
    for (std::size_t pos = 0; pos < o.pattern.size();) {
      const auto tail = std::string_view(o.pattern).substr(pos);
      if (tail.starts_with("{name}")) {
        append(stem);
        pos += 6;
      } else if (tail.starts_with("{number}")) {
        append(number);
        pos += 8;
      } else if (tail.starts_with("{{") || tail.starts_with("}}")) {
        append(tail.substr(0, 1));
        pos += 2;
      } else if (tail[0] == '{' || tail[0] == '}') {
        throw std::runtime_error("Use {name}, {number}, {{ or }} in the pattern.");
      } else {
        append(tail.substr(0, 1));
        ++pos;
      }
    }
  } else if (o.mode == BatchRenameMode::Replace) {
    if (o.find.empty()) {
      throw std::runtime_error("Enter text to find.");
    }
    std::size_t start = 0;
    for (;;) {
      const auto found = stem.find(o.find, start);
      if (found == std::string::npos) {
        append(std::string_view(stem).substr(start));
        break;
      }
      append(std::string_view(stem).substr(start, found - start));
      append(o.replacement);
      start = found + o.find.size();
    }
  } else {
    throw std::runtime_error("Unknown rename mode.");
  }
  append(extension);
  return result;
}

struct files::BatchRenameService::State {
  BatchRenameSnapshot snapshot;
  BatchRenameOptions options;
  std::shared_ptr<Prepared> prepared;
  std::atomic<bool> stop{false};
  std::shared_ptr<std::atomic<bool>> previewStop;
  std::atomic<std::size_t> completed{0};
  std::uint64_t version = 0;
  bool active = false;
  std::function<void()> changed;
  enum class Kind { Prepare, Preview, Apply };
  struct Reply {
    std::shared_ptr<Prepared> prepared;
    std::shared_ptr<std::vector<BatchRenameRow>> rows;
    std::shared_ptr<std::vector<BatchRenameResult>> results;
    std::string error;
    std::string report;
  };
  void notify() const {
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }
  static void launch(const std::shared_ptr<State>& owner, Kind kind, std::function<Reply()> work) {
    struct Request {
      std::shared_ptr<State> owner;
      Kind kind;
      std::uint64_t version;
      std::function<Reply()> work;
    };
    owner->active = true;
    const GioPtr<GTask> task(g_task_new(
        nullptr, nullptr,
        [](GObject*, GAsyncResult* result, gpointer) {
          const auto& request = *static_cast<Request*>(g_task_get_task_data(G_TASK(result)));
          const auto owner = request.owner;
          const std::unique_ptr<Reply> reply(static_cast<Reply*>(g_task_propagate_pointer(G_TASK(result), nullptr)));
          owner->active = false;
          if (request.kind != Kind::Apply && owner->stop.load()) {
            owner->snapshot.phase = BatchRenamePhase::Cancelled;
            owner->prepared.reset();
          } else if (request.kind == Kind::Preview && request.version != owner->version) {
            startPreview(owner);
            return;
          } else if (!reply->error.empty()) {
            owner->snapshot.phase = BatchRenamePhase::Error;
            owner->snapshot.error = reply->error;
          } else if (request.kind == Kind::Prepare) {
            owner->prepared = reply->prepared;
            owner->snapshot.parent = owner->prepared->parent;
            startPreview(owner);
            return;
          } else if (request.kind == Kind::Preview || !reply->results) {
            owner->snapshot.phase = BatchRenamePhase::Ready;
            owner->snapshot.rows = reply->rows;
            owner->snapshot.canApply = reply->rows && applicable(*reply->rows);
          } else {
            owner->snapshot.phase = BatchRenamePhase::Finished;
            owner->snapshot.results = reply->results;
            owner->snapshot.rows = reply->rows;
            owner->snapshot.report = std::move(reply->report);
            owner->prepared.reset();
          }
          owner->notify();
        },
        nullptr
    ));
    g_task_set_task_data(task.get(), new Request{owner, kind, owner->version, std::move(work)}, [](gpointer p) {
      delete static_cast<Request*>(p);
    });
    g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable*) {
      const auto& request = *static_cast<Request*>(data);
      auto reply = std::make_unique<Reply>();
      try {
        *reply = request.work();
      } catch (const std::exception& error) {
        reply->error = error.what();
      }
      g_task_return_pointer(task, reply.release(), [](gpointer p) { delete static_cast<Reply*>(p); });
    });
    owner->notify();
  }
  static void startPreview(const std::shared_ptr<State>& owner) {
    owner->snapshot.phase = BatchRenamePhase::Previewing;
    owner->snapshot.canApply = false;
    owner->previewStop = std::make_shared<std::atomic<bool>>(false);
    launch(owner, Kind::Preview, [plan = owner->prepared, options = owner->options, stop = owner->previewStop] {
      std::vector<std::string> names;
      for (std::size_t i = 0; i < plan->items.size() && !stop->load(); ++i) {
        const auto& entry = plan->items[i]->entry;
        names.push_back(
            batchRenameName(plan->items[i]->name, entry.type == G_FILE_TYPE_DIRECTORY && !entry.symlink, i, options)
        );
      }
      if (stop->load()) {
        return Reply{};
      }
      return Reply{.prepared = {}, .rows = validate(*plan, names, *stop), .results = {}, .error = {}, .report = {}};
    });
  }
};
files::BatchRenameService::BatchRenameService() : m_state(std::make_shared<State>()) {}
files::BatchRenameService::~BatchRenameService() {
  m_state->changed = {};
  cancel();
}
bool files::BatchRenameService::busy() const { return m_state->active; }
bool files::BatchRenameService::applying() const {
  return m_state->snapshot.phase == BatchRenamePhase::Applying && busy();
}
std::size_t files::BatchRenameService::completed() const { return m_state->completed.load(); }
const files::BatchRenameSnapshot& files::BatchRenameService::snapshot() const { return m_state->snapshot; }
void files::BatchRenameService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
void files::BatchRenameService::cancel() {
  m_state->stop.store(true);
  if (m_state->previewStop) {
    m_state->previewStop->store(true);
  }
  m_state->snapshot.canApply = false;
  if (!busy()) {
    m_state->prepared.reset();
    if (m_state->snapshot.phase != BatchRenamePhase::Finished) {
      m_state->snapshot.phase = BatchRenamePhase::Cancelled;
    }
  }
}
bool files::BatchRenameService::prepare(std::vector<DirectoryEntry> entries) {
  if (busy() || entries.size() < 2 || entries.size() > 4096) {
    return false;
  }
  m_state->prepared.reset();
  m_state->stop.store(false);
  m_state->completed.store(0);
  ++m_state->version;
  m_state->options = {};
  m_state->snapshot = {};
  m_state->snapshot.phase = BatchRenamePhase::Preparing;
  State::launch(m_state, State::Kind::Prepare, [owner = m_state, entries = std::move(entries)] mutable {
    return State::Reply{
        .prepared = prepareItems(std::move(entries), owner->stop), .rows = {}, .results = {}, .error = {}, .report = {}
    };
  });
  return true;
}
bool files::BatchRenameService::preview(BatchRenameOptions options) {
  if (applying()
      || m_state->stop.load()
      || (!m_state->prepared && m_state->snapshot.phase != BatchRenamePhase::Preparing)) {
    return false;
  }
  m_state->options = std::move(options);
  ++m_state->version;
  m_state->snapshot.canApply = false;
  m_state->snapshot.error.clear();
  if (m_state->previewStop) {
    m_state->previewStop->store(true);
  }
  if (!busy()) {
    State::startPreview(m_state);
  } else if (m_state->prepared) {
    m_state->snapshot.phase = BatchRenamePhase::Previewing;
  }
  return true;
}
bool files::BatchRenameService::apply() {
  if (busy() || !m_state->snapshot.canApply || !m_state->prepared) {
    return false;
  }
  m_state->snapshot.phase = BatchRenamePhase::Applying;
  m_state->snapshot.canApply = false;
  State::launch(
      m_state, State::Kind::Apply, [owner = m_state, plan = m_state->prepared, approved = m_state->snapshot.rows] {
        std::vector<std::string> names;
        for (const auto& row : *approved) {
          names.push_back(row.name);
        }
        auto rows = validate(*plan, names, owner->stop);
        if (!owner->stop.load() && !applicable(*rows)) {
          return State::Reply{.prepared = {}, .rows = std::move(rows), .results = {}, .error = {}, .report = {}};
        }
        auto results = execute(*plan, *rows, owner->stop, owner->completed);
        std::string report = "Batch rename in " + display(plan->path) + "\n";
        constexpr std::array labels{"Renamed", "Unchanged", "Failed", "Cancelled", "Needs attention", "Unconfirmed"};
        for (std::size_t i = 0; i < rows->size(); ++i) {
          const auto& result = (*results)[i];
          (*rows)[i].error = result.error;
          report +=
              (*rows)[i].before + " -> " + (*rows)[i].after + ": " + labels[static_cast<std::size_t>(result.outcome)];
          if (result.actual) {
            report += "\n  " + result.actual->uri();
          }
          if (!result.error.empty()) {
            report += "\n  " + result.error;
          }
          report += '\n';
        }
        return State::Reply{
            .prepared = {},
            .rows = std::move(rows),
            .results = std::move(results),
            .error = {},
            .report = std::move(report)
        };
      }
  );
  return true;
}
