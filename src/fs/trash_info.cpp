#include "fs/trash_info.h"

files::TrashInfo files::readTrashInfo(GFileInfo* info) {
  TrashInfo result;
  if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH) == G_FILE_ATTRIBUTE_TYPE_BYTE_STRING) {
    const char* path = g_file_info_get_attribute_byte_string(info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH);
    if (path != nullptr && g_path_is_absolute(path)) {
      result.original = Location::fromPath(path);
    }
  }
  if (g_file_info_get_attribute_type(info, G_FILE_ATTRIBUTE_TRASH_DELETION_DATE) == G_FILE_ATTRIBUTE_TYPE_STRING) {
    const char* date = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_TRASH_DELETION_DATE);
    if (date != nullptr && *date != '\0') {
      const GString valid(g_utf8_make_valid(date, -1));
      result.deletionDate = valid.get();
    }
  }
  return result;
}
std::string files::trashOriginalParentText(const TrashInfo& info) {
  if (!info.original) {
    return {};
  }
  const GioPtr<GFile> parent(g_file_get_parent(info.original->file().get()));
  const GString path(parent ? g_file_get_path(parent.get()) : nullptr);
  const GString display(path ? g_filename_display_name(path.get()) : nullptr);
  return display ? display.get() : "";
}
