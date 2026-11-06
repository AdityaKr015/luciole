#pragma once

#include "fs/directory_session.h"

namespace files {
  inline constexpr const char* trashItemAttributes =
      "standard::name,standard::display-name,standard::type,standard::is-symlink,standard::size,"
      "time::modified,id::file,id::filesystem,etag::value,trash::orig-path,trash::deletion-date";
  struct TrashItemIdentity {
    FileIdentity id;
    GFileType type;
    bool symlink;
    std::optional<std::string> etag, original, deleted;
    bool operator==(const TrashItemIdentity&) const = default;
  };
  bool isTopLevelTrashItem(const Location& location);
  TrashItemIdentity readTrashItemIdentity(GFileInfo* info);
  TrashItemIdentity readTrashItemIdentity(const DirectoryEntry& entry);
} // namespace files
