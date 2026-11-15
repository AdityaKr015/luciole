#pragma once

#include "fs/directory_entries.h"

#include <cstddef>
#include <gio/gio.h>
#include <string>
#include <vector>

namespace luciole {
  struct NativeListing {
    std::vector<DirectoryEntry> entries; // IDs are 0; the session assigns them on the main thread.
    bool unsupported = false;            // Use GIO's enumeration instead; it also reports the folder's errors.
    bool limited = false;                // entryLimit was reached.
  };

  // Lists a local folder with readdir and one statx per entry. Worker threads only.
  // Every fact equals what GIO's local backend reports for the session's attributes with NOFOLLOW_SYMLINKS, except
  // access read/write/execute, which are deferred to entryAccess(). Network and FUSE filesystems, unreadable entries
  // and any other failure report unsupported. A cancelled listing is empty.
  NativeListing listNativeFolder(const std::string& path, std::size_t entryLimit, GCancellable* cancellable);
} // namespace luciole
