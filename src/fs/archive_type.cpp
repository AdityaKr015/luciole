#include "fs/archive_type.h"

#include "fs/filename.h"
#include "fs/gio_ptr.h"

#include <algorithm>
#include <archive.h>

bool files::archiveCompressionAvailable(ArchiveCompression compression) {
  switch (compression) {
  case ArchiveCompression::None:
    return true;
  case ArchiveCompression::Gzip:
    return archive_zlib_version() != nullptr;
  case ArchiveCompression::Bzip2:
    return archive_bzlib_version() != nullptr;
  case ArchiveCompression::Zstd:
    return archive_libzstd_version() != nullptr;
  }
  return false;
}

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
  if (mime && (*mime == "application/zip" || *mime == "application/x-tar")) {
    return true;
  }
  if (mime && (*mime == "application/gzip" || *mime == "application/x-compressed-tar")) {
    return archiveCompressionAvailable(ArchiveCompression::Gzip);
  }
  if (mime && (*mime == "application/x-bzip2-compressed-tar" || *mime == "application/x-bzip-compressed-tar")) {
    return archiveCompressionAvailable(ArchiveCompression::Bzip2);
  }
  if (mime && *mime == "application/x-zstd-compressed-tar") {
    return archiveCompressionAvailable(ArchiveCompression::Zstd);
  }
  const auto name = location.basename();
  if (!name) {
    return false;
  }
  const files::GString lower(g_ascii_strdown(name->c_str(), -1));
  const std::string_view text(lower.get());
  if (text.ends_with(".zip") || text.ends_with(".tar")) {
    return true;
  }
  if (text.ends_with(".tar.gz") || text.ends_with(".tgz")) {
    return archiveCompressionAvailable(ArchiveCompression::Gzip);
  }
  if (text.ends_with(".tar.bz2") || text.ends_with(".tbz2") || text.ends_with(".tbz")) {
    return archiveCompressionAvailable(ArchiveCompression::Bzip2);
  }
  return (text.ends_with(".tar.zst") || text.ends_with(".tzst"))
      && archiveCompressionAvailable(ArchiveCompression::Zstd);
}
std::string files::archiveFolderName(const Location& location) {
  std::string name = location.basename().value_or("Archive");
  const files::GString lower(g_ascii_strdown(name.c_str(), -1));
  const std::string_view text(lower.get());
  for (const auto suffix :
       {std::string_view(".zip.001"), std::string_view(".tar.bz2"), std::string_view(".tar.zst"),
        std::string_view(".tar.gz"), std::string_view(".tbz2"), std::string_view(".tzst"), std::string_view(".tbz"),
        std::string_view(".tgz"), std::string_view(".zip"), std::string_view(".tar")}) {
    if (text.ends_with(suffix)) {
      name.resize(name.size() - suffix.size());
      break;
    }
  }
  return name.empty() || filenameProblem(name) ? "Archive" : name;
}
