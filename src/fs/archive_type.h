#pragma once

#include "fs/location.h"

#include <string_view>

namespace files {
  bool isSupportedArchive(const Location& location, const std::optional<std::string>& mime);
  std::string archiveFolderName(const Location& location);
} // namespace files
