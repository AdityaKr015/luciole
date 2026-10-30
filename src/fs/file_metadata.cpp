#include "fs/file_metadata.h"

namespace {
  std::optional<bool> boolean(GFileInfo* info, const char* attribute) {
    if (g_file_info_get_attribute_type(info, attribute) != G_FILE_ATTRIBUTE_TYPE_BOOLEAN) {
      return std::nullopt;
    }
    return g_file_info_get_attribute_boolean(info, attribute) != FALSE;
  }
} // namespace

std::optional<std::string> files::readFastContentType(GFileInfo* info) {
  if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE) != G_FILE_ATTRIBUTE_TYPE_STRING)
    return std::nullopt;
  const char* value = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
  if (value == nullptr || *value == '\0' || !g_utf8_validate(value, -1, nullptr))
    return std::nullopt;
  return value;
}

files::FileAccess files::readFileAccess(GFileInfo* info) {
  return {
      .read = boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ),
      .write = boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE),
      .execute = boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE),
      .rename = boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME),
      .trash = boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH),
      .remove = boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE)
  };
}

files::FilesystemInfo files::readFilesystemInfo(GFileInfo* info) {
  FilesystemInfo result{
      .readOnly = boolean(info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY),
      .remote = boolean(info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE),
      .type = std::nullopt
  };
  if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE) == G_FILE_ATTRIBUTE_TYPE_STRING) {
    result.type = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE);
  }
  if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE) == G_FILE_ATTRIBUTE_TYPE_UINT64) {
    result.freeBytes = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
  }
  return result;
}

std::optional<files::FileIdentity> files::readFileIdentity(GFileInfo* info) {
  if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM) != G_FILE_ATTRIBUTE_TYPE_STRING
      || g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_ID_FILE) != G_FILE_ATTRIBUTE_TYPE_STRING)
    return std::nullopt;
  const char* filesystem = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);
  const char* file = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILE);
  if (!filesystem || !*filesystem || !file || !*file)
    return std::nullopt;
  return FileIdentity{filesystem, file};
}
