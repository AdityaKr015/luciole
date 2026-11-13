#include "fs/directory_session.h"

#include "fs/gio_interop.h"

#include <unordered_map>
#include <utility>

namespace {
  constexpr const char* kAccessAttributes =
      G_FILE_ATTRIBUTE_ACCESS_CAN_READ "," G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE "," G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE
                                       "," G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME "," G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH
                                       "," G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE;
  constexpr const char* kFilesystemAttributes = G_FILE_ATTRIBUTE_FILESYSTEM_READONLY
      "," G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE "," G_FILE_ATTRIBUTE_FILESYSTEM_TYPE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE;
  constexpr const char* kAttributes = G_FILE_ATTRIBUTE_STANDARD_NAME
      "," G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE
      "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK
      "," G_FILE_ATTRIBUTE_STANDARD_ICON "," G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_TIME_MODIFIED
      "," G_FILE_ATTRIBUTE_TIME_ACCESS "," G_FILE_ATTRIBUTE_TIME_CREATED "," G_FILE_ATTRIBUTE_OWNER_USER
      "," G_FILE_ATTRIBUTE_OWNER_GROUP "," G_FILE_ATTRIBUTE_UNIX_MODE "," G_FILE_ATTRIBUTE_TIME_CHANGED
      ",time::changed-nsec," G_FILE_ATTRIBUTE_TIME_CHANGED_USEC "," G_FILE_ATTRIBUTE_ETAG_VALUE
      "," G_FILE_ATTRIBUTE_ACCESS_CAN_READ "," G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE "," G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE
      "," G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME "," G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH
      "," G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE "," G_FILE_ATTRIBUTE_TRASH_ORIG_PATH
      "," G_FILE_ATTRIBUTE_TRASH_DELETION_DATE "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE
      "," G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE "," G_FILE_ATTRIBUTE_ID_FILE "," G_FILE_ATTRIBUTE_ID_FILESYSTEM;
  constexpr int kBatchSize = 64;
  constexpr unsigned int kMonitorDelayMs = 80;

  void closeEnumerator(GFileEnumerator* enumerator) {
    if (enumerator == nullptr || g_file_enumerator_is_closed(enumerator)) {
      return;
    }
    g_file_enumerator_close_async(
        enumerator, G_PRIORITY_DEFAULT, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer) {
          g_file_enumerator_close_finish(G_FILE_ENUMERATOR(source), result, nullptr);
        },
        nullptr
    );
  }
} // namespace

struct luciole::DirectorySession::State : std::enable_shared_from_this<State> {
  std::size_t entryLimit = 0;
  struct Scan {
    GioPtr<GCancellable> cancellable{g_cancellable_new()};
    GioPtr<GFileEnumerator> enumerator;
    std::vector<DirectoryEntry> entries;
    DirectoryMetadata metadata;
    std::optional<FileError> accessError;
    std::optional<FileError> filesystemError;
    bool enumerating = true;
    bool accessPending = true;
    bool filesystemPending = true;
    ~Scan() { closeEnumerator(enumerator.get()); }
  };

  struct Request {
    std::weak_ptr<State> state;
    std::shared_ptr<Scan> scan;
  };

  DirectorySnapshot snapshot;
  std::function<void()> changed;
  std::shared_ptr<Scan> scan;
  GioPtr<GFileMonitor> monitor;
  std::unordered_map<std::string, std::uint64_t> identities;
  std::uint64_t nextId = 1;
  IdResolver resolveId;
  guint timer = 0;
  bool dirty = false;
  bool entriesAccessInvalidated = false;

  ~State() { stop(); }

  void notify() const {
    // A callback may replace itself, navigate, cancel, or destroy its session.
    const auto callback = changed;
    if (callback) {
      callback();
    }
  }

  void stopScan() {
    if (timer != 0) {
      g_source_remove(std::exchange(timer, 0));
    }
    if (scan) {
      g_cancellable_cancel(scan->cancellable.get());
      scan.reset();
    }
    dirty = false;
  }

  void stop() {
    stopScan();
    if (monitor) {
      g_file_monitor_cancel(monitor.get());
      monitor.reset();
    }
  }

  void invalidateMetadata(bool loading) {
    snapshot.metadata = {};
    snapshot.metadataLoading = loading;
    snapshot.accessError.reset();
    snapshot.filesystemError.reset();
    if (!entriesAccessInvalidated && !snapshot.entries.empty()) {
      std::vector<DirectoryEntry> entries(snapshot.entries.begin(), snapshot.entries.end());
      for (auto& entry : entries) {
        entry.access = {};
      }
      snapshot.entries = std::move(entries);
      entriesAccessInvalidated = true;
    }
  }

  void publishMetadata() {
    snapshot.metadata = scan->metadata;
    snapshot.metadataLoading = scan->accessPending || scan->filesystemPending;
    snapshot.accessError = scan->accessError;
    snapshot.filesystemError = scan->filesystemError;
  }

  void metadataCompleted() {
    if (!dirty && !scan->enumerating && snapshot.status == DirectoryStatus::Ready) {
      publishMetadata();
      notify();
    }
  }

  void queryMetadata() {
    const auto file = gioFile(*snapshot.location);
    g_file_query_info_async(
        file.get(), kAccessAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT, scan->cancellable.get(),
        [](GObject* source, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Request> request(static_cast<Request*>(data));
          GError* error = nullptr;
          const GioPtr<GFileInfo> info(g_file_query_info_finish(G_FILE(source), result, &error));
          if (const auto state = request->state.lock(); state && state->scan == request->scan) {
            request->scan->accessPending = false;
            if (info) {
              request->scan->metadata.access = readFileAccess(info.get());
            } else if (error != nullptr) {
              request->scan->accessError = fileError(*error);
            }
            state->metadataCompleted();
          }
          g_clear_error(&error);
        },
        new Request{.state = weak_from_this(), .scan = scan}
    );
    g_file_query_filesystem_info_async(
        file.get(), kFilesystemAttributes, G_PRIORITY_DEFAULT, scan->cancellable.get(),
        [](GObject* source, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Request> request(static_cast<Request*>(data));
          GError* error = nullptr;
          const GioPtr<GFileInfo> info(g_file_query_filesystem_info_finish(G_FILE(source), result, &error));
          if (const auto state = request->state.lock(); state && state->scan == request->scan) {
            request->scan->filesystemPending = false;
            if (info) {
              request->scan->metadata.filesystem = readFilesystemInfo(info.get());
            } else if (error != nullptr) {
              request->scan->filesystemError = fileError(*error);
            }
            state->metadataCompleted();
          }
          g_clear_error(&error);
        },
        new Request{.state = weak_from_this(), .scan = scan}
    );
  }

  void schedule() {
    if (timer != 0) {
      return;
    }
    timer = g_timeout_add_full(
        G_PRIORITY_DEFAULT, kMonitorDelayMs,
        [](gpointer data) -> gboolean {
          if (const auto state = static_cast<std::weak_ptr<State>*>(data)->lock()) {
            state->timer = 0;
            state->begin();
          }
          return G_SOURCE_REMOVE;
        },
        new std::weak_ptr<State>(weak_from_this()),
        [](gpointer data) { delete static_cast<std::weak_ptr<State>*>(data); }
    );
  }

  void monitorChanged(GFile* file, GFile* other, GFileMonitorEvent event) {
    const GString uri(g_file_get_uri(file));
    // Only an explicit same-directory rename proves continuity. Replacements
    // and delete/create sequences get a new identity even at the same URI.
    if (event == G_FILE_MONITOR_EVENT_RENAMED && other != nullptr) {
      const GString otherUri(g_file_get_uri(other));
      identities.erase(otherUri.get());
      if (auto node = identities.extract(uri.get()); !node.empty()) {
        node.key() = otherUri.get();
        identities.insert(std::move(node));
      }
    } else if (
        event == G_FILE_MONITOR_EVENT_DELETED
        || event == G_FILE_MONITOR_EVENT_MOVED_OUT
        || event == G_FILE_MONITOR_EVENT_CREATED
        || event == G_FILE_MONITOR_EVENT_MOVED_IN
    ) {
      identities.erase(uri.get());
    }
    const bool wasDirty = dirty;
    dirty = true;
    if (!scan || !scan->enumerating) {
      schedule();
      if (!wasDirty) {
        invalidateMetadata(true);
        notify();
      }
    }
  }

  void watch() {
    const auto file = gioFile(*snapshot.location);
    if (!g_file_is_native(file.get())) {
      snapshot.monitorError = FileError{
          .kind = FileErrorKind::Unsupported,
          .message = snapshot.location->isTrash() ? "Refresh to update the Trash view."
                                                  : "Remote change monitoring is not implemented yet."
      };
      return;
    }
    GError* error = nullptr;
    monitor.reset(g_file_monitor_directory(file.get(), G_FILE_MONITOR_WATCH_MOVES, nullptr, &error));
    if (error != nullptr) {
      snapshot.monitorError = fileError(*error);
      g_error_free(error);
      return;
    }
    g_signal_connect(
        monitor.get(), "changed",
        G_CALLBACK(+[](GFileMonitor*, GFile* file, GFile* other, GFileMonitorEvent event, gpointer data) {
          // Keep State alive if its observer destroys the session.
          const auto state = static_cast<State*>(data)->shared_from_this();
          state->monitorChanged(file, other, event);
        }),
        this
    );
  }

  void begin() {
    stopScan();
    ++snapshot.generation;
    snapshot.status = DirectoryStatus::Loading;
    snapshot.error.reset();
    invalidateMetadata(true);
    scan = std::make_shared<Scan>();
    queryMetadata();
    g_file_enumerate_children_async(
        gioFile(*snapshot.location).get(), kAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT,
        scan->cancellable.get(),
        [](GObject* source, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Request> request(static_cast<Request*>(data));
          GError* error = nullptr;
          request->scan->enumerator.reset(g_file_enumerate_children_finish(G_FILE(source), result, &error));
          if (const auto state = request->state.lock(); state && state->scan == request->scan) {
            if (error != nullptr) {
              state->complete(fileError(*error));
            } else {
              state->nextBatch();
            }
          }
          g_clear_error(&error);
        },
        new Request{.state = weak_from_this(), .scan = scan}
    );
    notify();
  }

  void append(GFileInfo* info) {
    const char* name = g_file_info_get_name(info);
    const auto parent = gioFile(*snapshot.location);
    const GioPtr<GFile> child(g_file_get_child(parent.get(), name));
    const GString uri(g_file_get_uri(child.get()));
    std::uint64_t id = 0;
    if (!resolveId) {
      auto [identity, inserted] = identities.try_emplace(uri.get(), nextId);
      if (inserted) {
        ++nextId;
      }
      id = identity->second;
    }
    DirectoryEntry entry{
        .id = id,
        .location = Location::fromUri(uri.get()),
        .name = name,
        .displayName = g_file_info_get_display_name(info),
        .type = fileType(g_file_info_get_file_type(info)),
        .hidden =
            g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN) != FALSE || name[0] == '.',
        .symlink = readIsSymlink(info) != FALSE,
        .size = std::nullopt,
        .modifiedSeconds = std::nullopt,
        .etag = std::nullopt,
        .access = readFileAccess(info),
        .parentDisplayName = {},
        .icon = readFileIcon(info, Location::fromUri(uri.get())),
    };
    if (entry.location.isTrash()) {
      entry.trash = readTrashInfo(info);
      entry.parentDisplayName = trashOriginalParentText(entry.trash);
    }
    entry.details = readEntryMetadata(info);
    entry.identity = readFileIdentity(info);
    entry.contentType = readContentType(info);
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE)) {
      const auto size = g_file_info_get_size(info);
      if (size >= 0) {
        entry.size = static_cast<std::uint64_t>(size);
      }
    }
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_CHANGED)) {
      const auto seconds = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_CHANGED);
      const auto nanos = g_file_info_has_attribute(info, "time::changed-nsec")
          ? g_file_info_get_attribute_uint32(info, "time::changed-nsec")
          : g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_TIME_CHANGED_USEC) * 1000U;
      entry.contentRevision = std::to_string(seconds) + ':' + std::to_string(nanos);
    }
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
      entry.modifiedSeconds = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    }
    if (const char* etag = g_file_info_get_etag(info); etag != nullptr) {
      entry.etag = etag;
    }
    if (resolveId) {
      entry.id = resolveId(entry);
    }
    scan->entries.push_back(std::move(entry));
  }

  void nextBatch() {
    g_file_enumerator_next_files_async(
        scan->enumerator.get(), kBatchSize, G_PRIORITY_DEFAULT, scan->cancellable.get(),
        [](GObject* source, GAsyncResult* result, gpointer data) {
          const std::unique_ptr<Request> request(static_cast<Request*>(data));
          GError* error = nullptr;
          GList* infos = g_file_enumerator_next_files_finish(G_FILE_ENUMERATOR(source), result, &error);
          if (const auto state = request->state.lock(); state && state->scan == request->scan) {
            if (error != nullptr) {
              state->complete(fileError(*error));
            } else if (infos == nullptr) {
              state->complete(std::nullopt);
            } else {
              bool limited = false;
              for (const GList* item = infos; item != nullptr; item = item->next) {
                if (state->entryLimit && state->scan->entries.size() >= state->entryLimit) {
                  limited = true;
                  break;
                }
                state->append(G_FILE_INFO(item->data));
              }
              if (limited) {
                state->complete(
                    FileError{
                        FileErrorKind::Unsupported, "Tree entry limit reached. Open this folder to see all items."
                    }
                );
              } else {
                state->nextBatch();
              }
            }
          }
          g_list_free_full(infos, g_object_unref);
          g_clear_error(&error);
        },
        new Request{.state = weak_from_this(), .scan = scan}
    );
  }

  void complete(std::optional<FileError> error) {
    if (dirty) {
      stopScan();
      schedule();
      return;
    }
    snapshot.entries = std::move(scan->entries);
    entriesAccessInvalidated = false;
    scan->enumerating = false;
    closeEnumerator(scan->enumerator.get());
    scan->enumerator.reset();
    if (!resolveId) {
      std::unordered_map<std::string, std::uint64_t> retained;
      retained.reserve(snapshot.entries.size());
      for (const auto& entry : snapshot.entries) {
        retained.emplace(entry.location.uri(), entry.id);
      }
      identities = std::move(retained);
    }
    snapshot.error = std::move(error);
    if (snapshot.location->isTrash() && snapshot.error && snapshot.error->kind == FileErrorKind::Unsupported) {
      snapshot.error->message = "Trash browsing is unavailable. The desktop trash service is not accessible.";
    }
    snapshot.status = snapshot.error ? DirectoryStatus::Error : DirectoryStatus::Ready;
    if (snapshot.error) {
      g_cancellable_cancel(scan->cancellable.get());
      invalidateMetadata(false);
    } else {
      publishMetadata();
    }
    notify();
  }
};

luciole::DirectorySession::DirectorySession(std::size_t entryLimit, IdResolver resolveId)
    : m_state(std::make_shared<State>()) {
  m_state->entryLimit = entryLimit;
  m_state->resolveId = std::move(resolveId);
}

luciole::DirectorySession::~DirectorySession() {
  m_state->changed = {};
  m_state->stop();
}

void luciole::DirectorySession::open(const Location& location) {
  const auto state = m_state;
  state->stop();
  state->identities.clear();
  state->snapshot.location = location;
  state->snapshot.entries = {};
  state->entriesAccessInvalidated = false;
  state->snapshot.monitorError.reset();
  state->watch();
  state->begin();
}

void luciole::DirectorySession::refresh() {
  const auto state = m_state;
  if (state->snapshot.location) {
    if (!state->monitor) {
      state->snapshot.monitorError.reset();
      state->watch();
      // Changes before this new watch were not observed.
      state->identities.clear();
    }
    state->begin();
  }
}

void luciole::DirectorySession::cancel() {
  const auto state = m_state;
  state->stop();
  state->identities.clear();
  ++state->snapshot.generation;
  state->snapshot.status = DirectoryStatus::Cancelled;
  state->snapshot.error.reset();
  state->invalidateMetadata(false);
  state->notify();
}

void luciole::DirectorySession::setChangedCallback(std::function<void()> callback) {
  m_state->changed = std::move(callback);
}

const luciole::DirectorySnapshot& luciole::DirectorySession::snapshot() const { return m_state->snapshot; }
