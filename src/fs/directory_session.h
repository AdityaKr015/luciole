#pragma once

#include "fs/file_error.h"
#include "fs/file_icon_source.h"
#include "fs/file_metadata.h"
#include "fs/location.h"
#include "fs/trash_info.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace files {
  enum class DirectoryStatus : std::uint8_t { Idle, Loading, Ready, Error, Cancelled };
  struct DirectoryEntry {
    std::uint64_t id = 0;
    Location location;
    std::string name; // Original filename bytes, including non-UTF-8 names on Unix.
    std::string displayName;
    GFileType type = G_FILE_TYPE_UNKNOWN;
    bool hidden = false;
    bool symlink = false;
    std::optional<std::uint64_t> size;
    std::optional<std::uint64_t> modifiedSeconds;
    std::optional<std::string> etag;
    FileAccess access{};
    std::string parentDisplayName; // Search result context; never an operation address.
    FileIconSource icon{};
    TrashInfo trash{};
    std::optional<FileIdentity> identity = std::nullopt;
    std::optional<std::string> contentType = std::nullopt;     // Provider's fast type; no content sniffing requested.
    std::optional<std::string> contentRevision = std::nullopt; // Provider change time, including subsecond precision.
  };

  struct DirectorySnapshot {
    std::uint64_t generation = 0;
    std::optional<Location> location;
    DirectoryStatus status = DirectoryStatus::Idle;
    std::vector<DirectoryEntry> entries;
    std::optional<FileError> error;
    std::optional<FileError> monitorError;
    DirectoryMetadata metadata{};
    bool metadataLoading = false;
    std::optional<FileError> accessError = std::nullopt;
    std::optional<FileError> filesystemError = std::nullopt;
    bool incrementalResults = false; // Fresh current-scan batches, rather than retained refresh entries.
  };

  // Main/default-context service. Observers run there and must not throw.
  // IDs last for this session; they are neither inodes nor persistent bookmarks.
  class DirectorySession {
  public:
    DirectorySession();
    ~DirectorySession();
    DirectorySession(const DirectorySession&) = delete;
    DirectorySession& operator=(const DirectorySession&) = delete;
    DirectorySession(DirectorySession&&) = delete;
    DirectorySession& operator=(DirectorySession&&) = delete;

    void open(const Location& location);
    void refresh();
    void cancel();
    void setChangedCallback(std::function<void()> callback);
    const DirectorySnapshot& snapshot() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
