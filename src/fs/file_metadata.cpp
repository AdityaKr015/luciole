#include "fs/file_metadata.h"

#include "fs/gio_interop.h"

std::optional<std::string> luciole::readContentType(GFileInfo* info) {
  for (const auto* attribute : {G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE}) {
    const char* value = infoString(info, attribute);
    if (value != nullptr && *value != '\0' && g_utf8_validate(value, -1, nullptr)) {
      return value;
    }
  }
  return std::nullopt;
}

std::string luciole::fileTypeDescription(FileType type, bool symlink, const std::optional<std::string>& contentType) {
  if (symlink || type == FileType::Symlink) {
    return "Link";
  }
  switch (type) {
  case FileType::Directory:
    return "Folder";
  case FileType::Special:
    return "Special";
  case FileType::Shortcut:
    return "Shortcut";
  case FileType::Mountable:
    return "Mount";
  case FileType::Regular:
    if (contentType && !g_content_type_is_unknown(contentType->c_str())) {
      const GString description(g_content_type_get_description(contentType->c_str()));
      return description && *description ? description.get() : *contentType;
    }
    return "File";
  default:
    return "Unknown";
  }
}

bool luciole::contentTypeIsText(const std::string& contentType) {
  if (contentType.starts_with("image/") || contentType.starts_with("video/") || contentType.starts_with("audio/")) {
    return false;
  }
  return !contentType.empty()
      && (contentType.starts_with("text/")
          || g_content_type_is_a(contentType.c_str(), "text/plain")
          // Retain the existing Nix-provider spelling even when absent from the installed MIME database.
          || contentType == "application/x-nix");
}

luciole::FileAccess luciole::readFileAccess(GFileInfo* info) {
  return {
      .read = infoBoolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ),
      .write = infoBoolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE),
      .execute = infoBoolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE),
      .rename = infoBoolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME),
      .trash = infoBoolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH),
      .remove = infoBoolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE)
  };
}

luciole::FilesystemInfo luciole::readFilesystemInfo(GFileInfo* info) {
  FilesystemInfo result{
      .readOnly = infoBoolean(info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY),
      .remote = infoBoolean(info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE),
      .type = std::nullopt
  };
  if (const char* type = infoString(info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE)) {
    result.type = type;
  }
  result.freeBytes = infoUint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
  return result;
}

std::optional<luciole::FileIdentity> luciole::readFileIdentity(GFileInfo* info) {
  const char* filesystem = infoString(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
  const char* file = infoString(info, G_FILE_ATTRIBUTE_ID_FILE);
  if (!filesystem || !*filesystem || !file || !*file) {
    return std::nullopt;
  }
  return FileIdentity{filesystem, file};
}

luciole::EntryMetadata luciole::readEntryMetadata(GFileInfo* info) {
  EntryMetadata result;
  const auto text = [&](const char* key) -> std::optional<std::string> {
    const char* value = infoString(info, key);
    if (!value || !*value || !g_utf8_validate(value, -1, nullptr)) {
      return {};
    }
    return value;
  };
  result.accessedSeconds = infoUint64(info, G_FILE_ATTRIBUTE_TIME_ACCESS);
  result.createdSeconds = infoUint64(info, G_FILE_ATTRIBUTE_TIME_CREATED);
  result.owner = text(G_FILE_ATTRIBUTE_OWNER_USER);
  result.group = text(G_FILE_ATTRIBUTE_OWNER_GROUP);
  if (const auto mode = infoUint32(info, G_FILE_ATTRIBUTE_UNIX_MODE)) {
    result.mode = *mode & 07777U;
  }
  return result;
}
