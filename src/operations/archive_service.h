#pragma once

#include "fs/archive_type.h"
#include "fs/file_error.h"
#include "fs/file_icon_source.h"
#include "operations/transfer_service.h"

#include <chrono>
#include <optional>
#include <span>
#include <string_view>

namespace files {
  enum class ArchiveFormat { Zip, Tar, TarGzip };
  enum class ArchiveEntryKind { File, Directory, Symlink, Hardlink };
  enum class ArchivePhase { Idle, Reading, Compressing, Extracting, Transferring, Cleaning };
  struct ArchiveFormatInfo {
    ArchiveFormat format;
    std::string_view label;
    std::string_view suffix;
  };
  struct ArchiveEntry {
    std::string path, parent, name, displayName;
    ArchiveEntryKind kind = ArchiveEntryKind::File;
    std::optional<std::uint64_t> size;
    std::string link;
    unsigned mode = 0644;
    FileIconSource icon{};
  };
  struct ArchiveCatalog {
    Location source;
    std::string stamp, format;
    std::vector<ArchiveEntry> entries;
    std::uint64_t expandedBytes = 0;
  };
  struct ArchiveLimits {
    std::uint64_t maxArchiveBytes = 4ULL * 1024 * 1024 * 1024;
    std::uint64_t maxExpandedBytes = 8ULL * 1024 * 1024 * 1024;
    std::size_t maxEntries = 50000;
    std::size_t maxMetadataBytes = 16U * 1024 * 1024;
    unsigned maxDepth = 128;
    std::chrono::seconds maxTime{120};
  };
  std::span<const ArchiveFormatInfo> archiveFormats();

  // One archive job, including staged publication through the existing transfer service.
  class ArchiveService {
  public:
    explicit ArchiveService(ArchiveLimits limits = {});
    ~ArchiveService();
    ArchiveService(const ArchiveService&) = delete;
    ArchiveService& operator=(const ArchiveService&) = delete;
    bool inspect(Location source);
    bool compress(std::vector<Location> sources, Location destinationFile, ArchiveFormat format);
    bool extract(Location source, Location destinationFolder, std::vector<std::string> selected = {});
    bool openMember(const std::string& path);
    void cancel();
    void shutdown();
    bool busy() const;
    ArchivePhase phase() const;
    std::uint64_t bytes() const;
    std::size_t items() const;
    const std::optional<FileError>& error() const;
    const std::shared_ptr<const ArchiveCatalog>& catalog() const;
    std::optional<Location> takeOpenedFile();
    const std::vector<TransferResult>& results() const;
    std::optional<TransferConflict> conflict() const;
    bool resolve(std::uint64_t id, TransferConflictChoice choice);
    void setChangedCallback(std::function<void()> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
