#pragma once

#include "fs/location.h"

namespace files {
  struct TrashInfo {
    std::optional<Location> original;
    std::optional<std::string> deletionDate;
  };
  TrashInfo readTrashInfo(GFileInfo* info);
  std::string trashOriginalParentText(const TrashInfo& info);
} // namespace files
