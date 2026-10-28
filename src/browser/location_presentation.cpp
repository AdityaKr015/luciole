#include "browser/location_presentation.h"

#include <algorithm>
#include <memory>

std::vector<files::Breadcrumb> files::locationBreadcrumbs(const Location& location) {
  std::vector<Breadcrumb> result;
  auto current = location.file();
  while (current) {
    const GString uri(g_file_get_uri(current.get()));
    auto parent = GioPtr<GFile>(g_file_get_parent(current.get()));
    std::string label;
    if (parent) {
      const GString name(g_file_get_basename(current.get()));
      const GString display(g_filename_display_name(name ? name.get() : uri.get()));
      label = display.get();
    } else if (g_file_is_native(current.get())) {
      label = "File System";
    } else {
      const std::unique_ptr<GUri, decltype(&g_uri_unref)> parsed(
          g_uri_parse(uri.get(), G_URI_FLAGS_ENCODED, nullptr), &g_uri_unref
      );
      const char* host = parsed ? g_uri_get_host(parsed.get()) : nullptr;
      label = host != nullptr && *host != '\0' ? host : "Location";
    }
    result.push_back({.location = Location::fromUri(uri.get()), .label = std::move(label)});
    if (parent && g_file_equal(current.get(), parent.get())) {
      break;
    }
    current = std::move(parent);
  }
  std::ranges::reverse(result);
  return result;
}

std::string files::locationEditText(const Location& location) {
  const auto path = location.nativePath();
  return path && g_utf8_validate(path->c_str(), static_cast<gssize>(path->size()), nullptr) ? *path : location.uri();
}
