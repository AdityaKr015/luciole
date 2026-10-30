#include "browser/thumbnail_service.h"

#include "browser/thumbnail_codec.h"
#include "browser/thumbnail_provider.h"
#include "browser/thumbnail_store.h"
#include "fs/gio_ptr.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <fcntl.h>
#include <format>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace {
  constexpr std::size_t kResidentBudget = 64U * 1024U * 1024U;
  constexpr std::size_t kMaxEntries = 1024;
  std::mutex diskMutex;
  struct Fd {
    int value;
    ~Fd() {
      if (value >= 0)
        ::close(value);
    }
  };
  std::string stamp(const struct stat& info) {
    return std::format(
        "{}:{}:{}:{}:{}:{}:{}", info.st_dev, info.st_ino, info.st_size, info.st_mtim.tv_sec, info.st_mtim.tv_nsec,
        info.st_ctim.tv_sec, info.st_ctim.tv_nsec
    );
  }
  bool privateUri(const std::string& uri) {
    const std::unique_ptr<GUri, decltype(&g_uri_unref)> parsed(
        g_uri_parse(uri.c_str(), static_cast<GUriFlags>(G_URI_FLAGS_HAS_PASSWORD | G_URI_FLAGS_ENCODED), nullptr),
        &g_uri_unref
    );
    return !parsed
        || g_uri_get_password(parsed.get())
        || g_uri_get_query(parsed.get())
        || g_uri_get_fragment(parsed.get());
  }
  files::FileIconImage
  load(const files::DirectoryEntry& source, int side, const files::ThumbnailOptions& options, GCancellable* cancel) {
    using namespace files;
    FileIconImage result{.pixels = std::nullopt, .path = {}, .symbolic = false, .ready = true};
    ThumbnailStore store;
    const auto native = source.location.nativePath();
    if (!native) {
      if (options.policy == ThumbnailPolicy::ALL && source.modifiedSeconds)
        if (auto cached = store.read(source.location.uri(), side, *source.modifiedSeconds, source.size))
          result.pixels = std::move(cached->image);
      return result;
    }
    if (g_cancellable_is_cancelled(cancel))
      return result;
    const GioPtr<GFileInfo> filesystem(
        g_file_query_filesystem_info(source.location.file().get(), G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, cancel, nullptr)
    );
    const bool local = filesystem
        && g_file_info_has_attribute(filesystem.get(), G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE)
        && !g_file_info_get_attribute_boolean(filesystem.get(), G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE);
    if (!local) {
      if (options.policy == ThumbnailPolicy::ALL && source.modifiedSeconds)
        if (auto cached = store.read(source.location.uri(), side, *source.modifiedSeconds, source.size))
          result.pixels = std::move(cached->image);
      return result;
    }
    const Fd file{::open(native->c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    struct stat before{};
    if (file.value < 0
        || ::fstat(file.value, &before) < 0
        || !S_ISREG(before.st_mode)
        || before.st_size <= 0
        || static_cast<std::uint64_t>(before.st_size) > static_cast<std::uint64_t>(options.maxFileMiB) * 1024U * 1024U)
      return result;
    if ((source.size && *source.size != static_cast<std::uint64_t>(before.st_size))
        || (source.modifiedSeconds && *source.modifiedSeconds != static_cast<std::uint64_t>(before.st_mtim.tv_sec)))
      return result;
    const GioPtr<GFileInfo> current(g_file_query_info(
        source.location.file().get(),
        G_FILE_ATTRIBUTE_ETAG_VALUE "," G_FILE_ATTRIBUTE_ID_FILE "," G_FILE_ATTRIBUTE_ID_FILESYSTEM,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, nullptr
    ));
    if (!current
        || (source.etag
            && (!g_file_info_get_etag(current.get()) || *source.etag != g_file_info_get_etag(current.get()))))
      return result;
    if (source.identity && readFileIdentity(current.get()) != source.identity)
      return result;
    const auto sourceStamp = stamp(before);
    const auto relative = std::filesystem::path(*native).lexically_relative(store.root());
    const bool insideCache = !relative.empty() && *relative.begin() != "..";
    if (!insideCache) {
      if (auto cached = store.read(
              source.location.uri(), side, static_cast<std::uint64_t>(before.st_mtim.tv_sec),
              static_cast<std::uint64_t>(before.st_size), sourceStamp
          )) {
        struct stat after{};
        if (::fstat(file.value, &after) == 0 && stamp(after) == sourceStamp && !g_cancellable_is_cancelled(cancel))
          result.pixels = std::move(cached->image);
        return result;
      }
    }
    std::expected<LoadedImageFile, std::string> decoded = std::unexpected("no thumbnail decoder");
    const auto mime = source.contentType.value_or("");
    if (mime.starts_with("image/") || !insideCache)
      decoded = renderThumbnailProvider(file.value, mime, side, options, cancel);
    struct stat after{};
    if (!decoded
        || ::fstat(file.value, &after) < 0
        || sourceStamp != stamp(after)
        || g_cancellable_is_cancelled(cancel))
      return result;
    // Reject replacement of the original pathname while the held descriptor was decoded.
    struct stat named{};
    if (::lstat(native->c_str(), &named) < 0 || stamp(named) != sourceStamp)
      return result;
    if (!insideCache) {
      ThumbnailPng cached{
          .image = *decoded,
          .text = {
              {"Thumb::URI", source.location.uri()},
              {"Thumb::MTime", std::to_string(before.st_mtim.tv_sec)},
              {"Thumb::Size", std::to_string(before.st_size)},
              {"X-Noctalia-Files::Stamp", sourceStamp}
          }
      };
      std::scoped_lock lock(diskMutex);
      store.write(std::move(cached), side, static_cast<std::uint64_t>(options.cacheMiB) * 1024U * 1024U, cancel);
    }
    result.pixels = std::move(*decoded);
    return result;
  }
} // namespace

struct files::ThumbnailService::State : std::enable_shared_from_this<State> {
  struct Item {
    explicit Item(DirectoryEntry entry) : source(std::move(entry)) {}
    std::shared_ptr<FileIconImage> image;
    DirectoryEntry source;
    GioPtr<GCancellable> cancel{g_cancellable_new()};
    std::uint64_t serial = 0, used = 0;
    int side = 128;
    bool visible = false, queued = true, failed = false, loading = false;
    std::chrono::steady_clock::time_point retry;
  };
  struct Request {
    std::shared_ptr<State> owner;
    std::string key;
    DirectoryEntry source;
    ThumbnailOptions options;
    std::uint64_t epoch, serial;
    int side;
  };
  struct Result {
    FileIconImage image;
    std::string error;
  };
  ThumbnailOptions options;
  std::unordered_map<std::string, Item> cache;
  std::function<void()> changed;
  std::uint64_t epoch = 0, clock = 0, serial = 0;
  std::size_t active = 0, reserved = 0;
  bool alive = true, clearing = false;
  std::string error;
  GSource* idle = nullptr;
  std::vector<GioPtr<GCancellable>> running;
  void notify() {
    const auto callback = changed;
    if (alive && callback)
      callback();
  }
  void retire() {
    ++epoch;
    for (const auto& cancel : running)
      g_cancellable_cancel(cancel.get());
    for (const auto& [_, item] : cache) {
      g_cancellable_cancel(item.cancel.get());
      item.image->pixels.reset();
      item.image->ready = false;
    }
    cache.clear();
    reserved = 0;
    if (idle) {
      g_source_destroy(idle);
      g_source_unref(std::exchange(idle, nullptr));
    }
  }
  void erase(std::unordered_map<std::string, Item>::iterator item) {
    g_cancellable_cancel(item->second.cancel.get());
    item->second.image->pixels.reset();
    item->second.image->ready = false;
    reserved -= static_cast<std::size_t>(item->second.side) * item->second.side * 4;
    cache.erase(item);
  }
  bool reserve(std::size_t bytes, bool visible) {
    while (reserved + bytes > kResidentBudget || cache.size() >= kMaxEntries) {
      auto oldest = cache.end();
      for (auto it = cache.begin(); it != cache.end(); ++it)
        if ((it->second.image.use_count() == 1 || (visible && !it->second.visible))
            && (oldest == cache.end() || it->second.used < oldest->second.used))
          oldest = it;
      if (oldest == cache.end())
        return false;
      erase(oldest);
    }
    reserved += bytes;
    return true;
  }
  void schedule() {
    if (idle || !alive || clearing)
      return;
    idle = g_idle_source_new();
    g_source_set_callback(
        idle,
        [](gpointer data) -> gboolean {
          auto* owner = static_cast<State*>(data);
          g_source_unref(std::exchange(owner->idle, nullptr));
          owner->pump();
          return G_SOURCE_REMOVE;
        },
        this, nullptr
    );
    g_source_attach(idle, g_main_context_default());
  }
  void pump() {
    while (alive && !clearing && active < 2) {
      auto next = cache.end();
      for (auto it = cache.begin(); it != cache.end();) {
        if ((it->second.queued || it->second.loading) && it->second.image.use_count() == 1) {
          const auto old = it++;
          erase(old);
          continue;
        }
        if (it->second.queued
            && (next == cache.end()
                || (it->second.visible && !next->second.visible)
                || (it->second.visible == next->second.visible && it->second.used < next->second.used)))
          next = it;
        ++it;
      }
      if (next == cache.end())
        return;
      auto& item = next->second;
      item.queued = false;
      item.loading = true;
      ++active;
      running.emplace_back(static_cast<GCancellable*>(g_object_ref(item.cancel.get())));
      GioPtr<GTask> task(g_task_new(
          nullptr, item.cancel.get(),
          [](GObject*, GAsyncResult* result, gpointer) {
            auto* task = G_TASK(result);
            const auto* request = static_cast<Request*>(g_task_get_task_data(task));
            const auto owner = request->owner;
            GError* error = nullptr;
            const std::unique_ptr<Result> loaded(static_cast<Result*>(g_task_propagate_pointer(task, &error)));
            g_clear_error(&error);
            --owner->active;
            std::erase_if(owner->running, [&](const auto& cancel) {
              return cancel.get() == g_task_get_cancellable(task);
            });
            auto found = owner->cache.find(request->key);
            if (owner->alive
                && owner->epoch == request->epoch
                && found != owner->cache.end()
                && found->second.serial == request->serial
                && loaded) {
              found->second.loading = false;
              *found->second.image = std::move(loaded->image);
              found->second.failed = !found->second.image->pixels;
              found->second.retry = std::chrono::steady_clock::now() + std::chrono::seconds(30);
              if (!loaded->error.empty())
                owner->error = loaded->error;
            }
            owner->pump();
            owner->notify();
          },
          nullptr
      ));
      g_task_set_task_data(
          task.get(), new Request{shared_from_this(), next->first, item.source, options, epoch, item.serial, item.side},
          [](gpointer data) { delete static_cast<Request*>(data); }
      );
      g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable* cancel) {
        const auto& request = *static_cast<Request*>(data);
        auto result = std::make_unique<Result>();
        try {
          result->image = load(request.source, request.side, request.options, cancel);
        } catch (const std::exception& error) {
          result->error = error.what();
          result->image.ready = true;
        }
        g_task_return_pointer(task, result.release(), [](gpointer data) { delete static_cast<Result*>(data); });
      });
    }
  }
};

files::ThumbnailService::ThumbnailService() : m_state(std::make_shared<State>()) {}
files::ThumbnailService::~ThumbnailService() {
  m_state->alive = false;
  m_state->changed = {};
  m_state->retire();
}
std::shared_ptr<const files::FileIconImage>
files::ThumbnailService::request(const DirectoryEntry& source, int pixels, bool visible) {
  auto& state = *m_state;
  if (state.options.policy == ThumbnailPolicy::NEVER
      || state.clearing
      || source.type != G_FILE_TYPE_REGULAR
      || source.symlink
      || !source.modifiedSeconds
      || privateUri(source.location.uri())
      || (state.options.policy == ThumbnailPolicy::LOCAL && !source.location.nativePath()))
    return {};
  const int side = ThumbnailStore::sizeClass(std::clamp(pixels, 1, 1024));
  const auto key = std::format(
      "{}\n{}\n{}\n{}\n{}\n{}", source.location.uri(), source.id, source.etag.value_or(""),
      source.modifiedSeconds.value_or(0), source.size.value_or(0), side
  );
  auto found = state.cache.find(key);
  if (found != state.cache.end() && found->second.failed && std::chrono::steady_clock::now() >= found->second.retry) {
    state.erase(found);
    found = state.cache.end();
  }
  if (found == state.cache.end()) {
    if (!state.reserve(static_cast<std::size_t>(side) * side * 4, visible))
      return {};
    State::Item item(source);
    item.image = std::make_shared<FileIconImage>();
    item.source = source;
    item.serial = ++state.serial;
    item.side = side;
    found = state.cache.emplace(key, std::move(item)).first;
  }
  found->second.visible = visible;
  found->second.used = ++state.clock;
  if (found->second.queued)
    state.schedule();
  return found->second.image;
}
void files::ThumbnailService::setOptions(ThumbnailOptions options) {
  if (options == m_state->options)
    return;
  const bool trim = options.cacheMiB < m_state->options.cacheMiB;
  m_state->retire();
  m_state->options = options;
  m_state->error.clear();
  if (trim) {
    const GioPtr<GTask> task(g_task_new(nullptr, nullptr, nullptr, nullptr));
    g_task_set_task_data(
        task.get(), new std::uint64_t(static_cast<std::uint64_t>(options.cacheMiB) * 1024U * 1024U),
        [](gpointer data) { delete static_cast<std::uint64_t*>(data); }
    );
    g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer data, GCancellable*) {
      try {
        std::scoped_lock lock(diskMutex);
        (void)ThumbnailStore{}.prune(*static_cast<std::uint64_t*>(data));
      } catch (const std::exception&) {
        // Cache pruning never prevents the requested browsing policy from applying.
      }
      g_task_return_boolean(task, true);
    });
  }
  m_state->notify();
}
void files::ThumbnailService::invalidate() { m_state->retire(); }
void files::ThumbnailService::endFrame() { m_state->schedule(); }
void files::ThumbnailService::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}
void files::ThumbnailService::clearCache() {
  if (m_state->clearing)
    return;
  m_state->retire();
  m_state->clearing = true;
  m_state->error.clear();
  const auto owner = m_state;
  GioPtr<GTask> task(g_task_new(
      nullptr, nullptr,
      [](GObject*, GAsyncResult* result, gpointer) {
        const auto owner = *static_cast<std::shared_ptr<State>*>(g_task_get_task_data(G_TASK(result)));
        const GString error(static_cast<char*>(g_task_propagate_pointer(G_TASK(result), nullptr)));
        owner->clearing = false;
        owner->error = error ? error.get() : "";
        owner->notify();
      },
      nullptr
  ));
  g_task_set_task_data(task.get(), new std::shared_ptr<State>(owner), [](gpointer data) {
    delete static_cast<std::shared_ptr<State>*>(data);
  });
  g_task_run_in_thread(task.get(), [](GTask* task, gpointer, gpointer, GCancellable*) {
    std::string error;
    try {
      std::scoped_lock lock(diskMutex);
      error = ThumbnailStore{}.clear();
    } catch (const std::exception& exception) {
      error = exception.what();
    }
    g_task_return_pointer(task, g_strdup(error.c_str()), g_free);
  });
  m_state->notify();
}
bool files::ThumbnailService::clearing() const { return m_state->clearing; }
const std::string& files::ThumbnailService::error() const { return m_state->error; }
std::size_t files::ThumbnailService::reservedBytes() const { return m_state->reserved; }
std::size_t files::ThumbnailService::activeJobs() const { return m_state->active; }
std::size_t files::ThumbnailService::queuedJobs() const {
  return static_cast<std::size_t>(std::ranges::count_if(m_state->cache, [](const auto& item) {
    return item.second.queued;
  }));
}
