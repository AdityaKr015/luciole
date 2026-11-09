#include "fs/archive_type.h"

#include "fs/filename.h"
#include "fs/gio_ptr.h"

#include <algorithm>

std::optional<unsigned> files::splitZipPart(const Location& location) {
  const auto name = location.basename();
  if (!name || name->size() < 8) {
    return std::nullopt;
  }
  const files::GString lower(g_ascii_strdown(name->c_str(), -1));
  const std::string_view text(lower.get());
  const auto index = text.substr(text.size() - 3);
  if (!text.substr(0, text.size() - 3).ends_with(".zip.")
      || !std::ranges::all_of(index, [](char c) { return c >= '0' && c <= '9'; })) {
    return std::nullopt;
  }
  return static_cast<unsigned>((index[0] - '0') * 100 + (index[1] - '0') * 10 + index[2] - '0');
}

bool files::isSupportedArchive(const Location& location, const std::optional<std::string>& mime) {
  if (!location.nativePath()) {
    return false;
  }
  if (const auto part = splitZipPart(location)) {
    return *part == 1;
  }
  if (mime
      && (*mime == "application/zip"
          || *mime == "application/x-tar"
          || *mime == "application/gzip"
          || *mime == "application/x-compressed-tar")) {
    return true;
  }
  const auto name = location.basename();
  if (!name) {
    return false;
  }
  const files::GString lower(g_ascii_strdown(name->c_str(), -1));
  const std::string_view text(lower.get());
  return text.ends_with(".zip") || text.ends_with(".tar") || text.ends_with(".tar.gz") || text.ends_with(".tgz");
}
std::string files::archiveFolderName(const Location& location) {
  std::string name = location.basename().value_or("Archive");
  const files::GString lower(g_ascii_strdown(name.c_str(), -1));
  const std::string_view text(lower.get());
  for (const auto suffix :
       {std::string_view(".zip.001"), std::string_view(".tar.gz"), std::string_view(".tgz"), std::string_view(".zip"),
        std::string_view(".tar")}) {
    if (text.ends_with(suffix)) {
      name.resize(name.size() - suffix.size());
      break;
    }
  }
  return name.empty() || filenameProblem(name) ? "Archive" : name;
}
