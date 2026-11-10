#pragma once

#include "fs/location.h"

#include <string>
#include <vector>

namespace luciole {
  struct FileIconSource {
    std::vector<std::string> names;
    std::string filePath;
    std::string desktopFile;
    bool operator==(const FileIconSource&) const = default;
  };
} // namespace luciole
