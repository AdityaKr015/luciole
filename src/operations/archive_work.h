#pragma once

#include "fs/cancel_token.h"
#include "fs/file_error.h"
#include "operations/archive_service.h"

#include <atomic>

namespace files::archive_work {
  struct Progress {
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::size_t> items{0};
  };
  struct Prepared {
    std::string stage;
    std::shared_ptr<ArchiveCatalog> catalog;
    std::optional<Location> output;
    std::uint64_t expandedBytes = 0;
  };
  // Entry icons are left empty; see archive_icons.h.
  Prepared inspect(const Location& source, ArchiveLimits limits, const CancelToken& cancel, Progress& progress);
  Prepared compress(
      const std::vector<Location>& sources, const Location& destination, ArchiveFormat format, ArchiveLimits limits,
      const CancelToken& cancel, Progress& progress
  );
  Prepared extract(
      const Location& source, const Location& destination, const std::vector<std::string>& selected,
      const std::shared_ptr<const ArchiveCatalog>& expected, bool open, ArchiveLimits limits, const CancelToken& cancel,
      Progress& progress
  );
  std::optional<FileError> cleanup(const std::vector<std::string>& paths);
} // namespace files::archive_work
