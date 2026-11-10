#pragma once

#include "fs/cancel_token.h"
#include "fs/file_error.h"
#include "fs/file_icon_source.h"
#include "fs/file_metadata.h"
#include "fs/gio_ptr.h"
#include "fs/location.h"
#include "fs/trash_info.h"

#include <gio/gio.h>
#include <optional>
#include <string>

// GIO bridge for code that drives GIO itself: fs, operations, thumbnails, imaging,
// platform, and clipboard sources. browser, config, ui, and app use the typed fs API.
namespace luciole {
  GioPtr<GFile> gioFile(const Location& location);
  Location locationFromFile(GFile* file);
  GCancellable* gioCancellable(const CancelToken& token);

  FileError fileError(const GError& error);
  FileType fileType(GFileType type);
  // Providers may omit the separate boolean when their type already describes the item.
  bool readIsSymlink(GFileInfo* info);

  std::optional<FileIdentity> readFileIdentity(GFileInfo* info);
  EntryMetadata readEntryMetadata(GFileInfo* info);
  FileAccess readFileAccess(GFileInfo* info);
  FilesystemInfo readFilesystemInfo(GFileInfo* info);
  std::optional<std::string> readFastContentType(GFileInfo* info);
  FileIconSource readIconSource(GIcon* icon);
  FileIconSource readFileIcon(GFileInfo* info, const Location& location);
  TrashInfo readTrashInfo(GFileInfo* info);
} // namespace luciole
