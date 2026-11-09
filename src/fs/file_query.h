#pragma once

#include "fs/cancel_token.h"
#include "fs/file_error.h"
#include "fs/file_metadata.h"
#include "fs/location.h"

#include <expected>
#include <functional>
#include <optional>
#include <string>

namespace files {
  struct FileDetails {
    FileType type = FileType::Unknown;
    std::optional<std::string> displayName;
    std::optional<std::string> typeDescription;
    std::optional<std::uint64_t> size;
    std::optional<std::uint64_t> modifiedSeconds;
    std::optional<FileIdentity> identity;
    std::optional<std::string> linkTarget;
    EntryMetadata metadata{};
    std::optional<std::string> etag = std::nullopt, contentRevision = std::nullopt;
  };
  // Blocking typed query for workers. Never follows links or reads file contents.
  FileDetails readFileDetails(const Location& location, const CancelToken& token);

  // Queries one item without following links or enumerating children. No content sniffing.
  // Cancelled replies are suppressed; done runs on the initiating main context.
  void queryFileDetails(
      const Location& location, const CancelToken& token,
      std::move_only_function<void(std::expected<FileDetails, FileError>)> done
  );
  // Follows symlinks. done runs on the main context unless token was cancelled first.
  void queryFileType(
      const Location& location, const CancelToken& token,
      std::move_only_function<void(std::expected<FileType, FileError>)> done
  );

  struct ChildInfo {
    std::string name; // Filename bytes.
    std::string displayName;
    FileType type = FileType::Unknown;
    bool hidden = false;
  };

  // Blocking; run on a worker. visit returns false to stop early. Cancellation is not an error.
  std::optional<FileError> enumerateChildren(
      const Location& parent, const CancelToken& token, const std::function<bool(const ChildInfo&)>& visit
  );
} // namespace files
