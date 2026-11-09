#pragma once

#include "fs/cancel_token.h"
#include "fs/folder_totals.h"

namespace files {
  struct FolderTotalsLimits {
    std::size_t items = 1000000, depth = 128;
  };
  // Worker entry point. Progress is a bounded value, never a retained per-item result list.
  FolderTotalsSnapshot walkFolderTotals(
      std::vector<DirectoryEntry> roots, const CancelToken& token,
      const std::function<void(const FolderTotalsSnapshot&)>& progress, FolderTotalsLimits limits = {}
  );
} // namespace files
