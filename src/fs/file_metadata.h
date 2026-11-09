#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace files {
  enum class FileType : std::uint8_t { Unknown, Regular, Directory, Symlink, Special, Shortcut, Mountable };

  // Provider-reported access facts, not promises that an operation will succeed.
  // nullopt is unknown; false is a reported negative, not a missing attribute.
  struct FileIdentity {
    std::string filesystem;
    std::string file;
    bool operator==(const FileIdentity&) const = default;
  };

  struct EntryMetadata {
    std::optional<std::uint64_t> accessedSeconds, createdSeconds;
    std::optional<std::string> owner, group;
    std::optional<std::uint32_t> mode;
    bool operator==(const EntryMetadata&) const = default;
  };
  struct FileAccess {
    std::optional<bool> read;
    std::optional<bool> write;
    std::optional<bool> execute;
    std::optional<bool> rename;
    std::optional<bool> trash;
    std::optional<bool> remove;
  };

  struct FilesystemInfo {
    std::optional<bool> readOnly;
    std::optional<bool> remote;
    std::optional<std::string> type;
    std::optional<std::uint64_t> freeBytes = std::nullopt;
  };

  struct DirectoryMetadata {
    FileAccess access;
    FilesystemInfo filesystem;
  };
} // namespace files
