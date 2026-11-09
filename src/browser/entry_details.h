#pragma once

#include "fs/directory_session.h"
#include "model/directory_options.h"

#include <string_view>

namespace files {
  std::string entryColumnText(const DirectoryEntry& entry, DetailsColumn column, std::string_view typeLabel = {});
  std::string metadataTime(std::optional<std::uint64_t> seconds);
  std::optional<std::uint64_t> entrySortNumber(const DirectoryEntry& entry, SortField field);
} // namespace files
