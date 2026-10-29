#pragma once

#include <cstdint>
#include <gio/gio.h>
#include <optional>
#include <string>

namespace files {
  // Provider-reported access facts, not promises that an operation will succeed.
  // nullopt is unknown; false is a reported negative, not a missing attribute.
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

  [[nodiscard]] FileAccess readFileAccess(GFileInfo* info);
  [[nodiscard]] FilesystemInfo readFilesystemInfo(GFileInfo* info);
} // namespace files
