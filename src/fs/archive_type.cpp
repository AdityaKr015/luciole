#include "fs/archive_type.h"

#include "fs/filename.h"
#include "fs/gio_ptr.h"

bool files::isSupportedArchive(const Location& location, const std::optional<std::string>& mime) {
  if (!location.nativePath()) {
    return false;
  }
  if (mime
      && (*mime == "application/zip"
          || *mime == "application/x-tar"
          || *mime == "application/gzip"
          || *mime == "application/x-compressed-tar")) {
    return true;
  }
  const files::GString name(g_file_get_basename(location.file().get()));
  if (!name) {
    return false;
  }
  const files::GString lower(g_ascii_strdown(name.get(), -1));
  const std::string_view text(lower.get());
  return text.ends_with(".zip") || text.ends_with(".tar") || text.ends_with(".tar.gz") || text.ends_with(".tgz");
}
std::string files::archiveFolderName(const Location& location) {
  const files::GString raw(g_file_get_basename(location.file().get()));
  std::string name = raw ? raw.get() : "Archive";
  const files::GString lower(g_ascii_strdown(name.c_str(), -1));
  const std::string_view text(lower.get());
  for (const auto suffix :
       {std::string_view(".tar.gz"), std::string_view(".tgz"), std::string_view(".zip"), std::string_view(".tar")}) {
    if (text.ends_with(suffix)) {
      name.resize(name.size() - suffix.size());
      break;
    }
  }
  return name.empty() || filenameProblem(name) ? "Archive" : name;
}
