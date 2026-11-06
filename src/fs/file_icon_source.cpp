#include "fs/file_icon_source.h"

namespace {
  void readIcon(GIcon* icon, files::FileIconSource& result) {
    if (icon == nullptr) {
      return;
    }
    if (G_IS_EMBLEMED_ICON(icon)) {
      readIcon(g_emblemed_icon_get_icon(G_EMBLEMED_ICON(icon)), result);
    } else if (G_IS_THEMED_ICON(icon)) {
      const auto* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
      for (std::size_t i = 0; names != nullptr && names[i] != nullptr; ++i) {
        result.names.emplace_back(names[i]);
      }
    } else if (G_IS_FILE_ICON(icon)) {
      const files::GString path(g_file_get_path(g_file_icon_get_file(G_FILE_ICON(icon))));
      if (path) {
        result.filePath = path.get();
      }
    }
  }
} // namespace

files::FileIconSource files::readIconSource(GIcon* icon) {
  FileIconSource result;
  readIcon(icon, result);
  return result;
}

files::FileIconSource files::readFileIcon(GFileInfo* info, const Location& location) {
  auto result = readIconSource(g_file_info_get_icon(info));
  const auto type = g_file_info_get_file_type(info);
  result.names.emplace_back(type == G_FILE_TYPE_DIRECTORY ? "folder" : "text-x-generic");
  result.names.emplace_back("application-octet-stream");
  const char* name = g_file_info_get_name(info);
  if (type == G_FILE_TYPE_REGULAR && name != nullptr && std::string_view(name).ends_with(".desktop")) {
    if (const auto path = location.nativePath()) {
      result.desktopFile = *path;
    }
  }
  return result;
}
