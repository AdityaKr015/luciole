#pragma once

#include "fs/location.h"

#include <string>
#include <vector>

namespace files {
  struct FileIconSource {
    std::vector<std::string> names;
    std::string filePath;
    std::string desktopFile;
    bool operator==(const FileIconSource&) const = default;
  };

  [[nodiscard]] FileIconSource readFileIcon(GFileInfo* info, const Location& location);
} // namespace files
