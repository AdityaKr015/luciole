#pragma once

#include "fs/location.h"

#include <string_view>

namespace files {
  // A byte-split ZIP part named <name>.zip.NNN; only part 001 opens the set.
  std::optional<unsigned> splitZipPart(const Location& location);
  bool isSupportedArchive(const Location& location, const std::optional<std::string>& mime);
  std::string archiveFolderName(const Location& location);
} // namespace files
