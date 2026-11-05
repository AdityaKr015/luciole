#include "fs/location.h"

#include <stdexcept>
#include <utility>

namespace {
  bool trashUri(const std::string& uri) { return g_ascii_strncasecmp(uri.c_str(), "trash:", 6) == 0; }

  files::GioPtr<GFile> fileForUri(const std::string& uri) {
    if (trashUri(uri)) {
      // Keep GVfs's interned Trash mount alive across UI/worker handle churn.
      static const files::GioPtr<GFile> root(g_file_new_for_uri("trash:///"));
      if (uri == "trash:///") {
        return files::GioPtr<GFile>(G_FILE(g_object_ref(root.get())));
      }
    }
    return files::GioPtr<GFile>(g_file_new_for_uri(uri.c_str()));
  }

  void rejectNul(const std::string& value) {
    if (value.contains('\0')) {
      throw std::invalid_argument("locations cannot contain a NUL byte");
    }
  }
} // namespace

files::Location::Location(std::string uri) : m_uri(std::move(uri)) {}

files::Location files::Location::fromUri(const std::string& uri) {
  rejectNul(uri);
  const GString scheme(g_uri_parse_scheme(uri.c_str()));
  if (!scheme || !g_uri_is_valid(uri.c_str(), G_URI_FLAGS_ENCODED, nullptr)) {
    throw std::invalid_argument("expected an absolute URI");
  }
  const auto file = fileForUri(uri);
  const GString canonical(g_file_get_uri(file.get()));
  return Location(canonical.get());
}

files::Location files::Location::fromPath(const std::string& absolutePath) {
  rejectNul(absolutePath);
  if (!g_path_is_absolute(absolutePath.c_str())) {
    throw std::invalid_argument("expected an absolute native path");
  }
  const GioPtr<GFile> file(g_file_new_for_path(absolutePath.c_str()));
  const GString uri(g_file_get_uri(file.get()));
  return Location(uri.get());
}

std::optional<std::string> files::Location::nativePath() const {
  if (isTrash()) {
    return std::nullopt;
  }
  const GString path(g_file_get_path(file().get()));
  return path ? std::optional<std::string>(path.get()) : std::nullopt;
}

files::Location files::Location::trash() { return Location("trash:///"); }
bool files::Location::isTrash() const { return trashUri(m_uri); }

files::GioPtr<GFile> files::Location::file() const { return fileForUri(m_uri); }
