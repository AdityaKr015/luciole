#include "operations/trash_item.h"

#include <stdexcept>

bool files::isTopLevelTrashItem(const Location& location) {
  const auto file = location.file();
  const GioPtr<GFile> parent(g_file_get_parent(file.get()));
  return location.isTrash() && parent && g_file_equal(parent.get(), Location::trash().file().get());
}
files::TrashItemIdentity files::readTrashItemIdentity(GFileInfo* info) {
  const auto id = readFileIdentity(info);
  const auto type = g_file_info_get_file_type(info);
  if (!id || type == G_FILE_TYPE_UNKNOWN) {
    throw std::runtime_error("The trash service cannot identify these items safely.");
  }
  const auto attribute = [info](const char* name, bool bytes = false) -> std::optional<std::string> {
    if (g_file_info_get_attribute_type(info, name)
        != (bytes ? G_FILE_ATTRIBUTE_TYPE_BYTE_STRING : G_FILE_ATTRIBUTE_TYPE_STRING)) {
      return std::nullopt;
    }
    const char* value =
        bytes ? g_file_info_get_attribute_byte_string(info, name) : g_file_info_get_attribute_string(info, name);
    return value && *value ? std::optional<std::string>(value) : std::nullopt;
  };
  return {
      *id,
      type,
      g_file_info_get_is_symlink(info) != FALSE,
      attribute(G_FILE_ATTRIBUTE_ETAG_VALUE),
      attribute(G_FILE_ATTRIBUTE_TRASH_ORIG_PATH, true),
      attribute(G_FILE_ATTRIBUTE_TRASH_DELETION_DATE)
  };
}
files::TrashItemIdentity files::readTrashItemIdentity(const DirectoryEntry& entry) {
  if (!entry.identity || entry.type == G_FILE_TYPE_UNKNOWN) {
    throw std::runtime_error("The trash service cannot identify these items safely.");
  }
  return {
      *entry.identity,
      entry.type,
      entry.symlink,
      entry.etag,
      entry.trash.original ? entry.trash.original->nativePath() : std::nullopt,
      entry.trash.deletionDate
  };
}
