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
