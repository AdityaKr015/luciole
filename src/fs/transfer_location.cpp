#include "fs/gio_interop.h"
#include "fs/transfer_types.h"

#include <string_view>

bool files::transferLocationSupported(const Location& location) {
  const std::unique_ptr<GUri, decltype(&g_uri_unref)> uri(
      g_uri_parse(
          location.uri().c_str(), static_cast<GUriFlags>(G_URI_FLAGS_ENCODED | G_URI_FLAGS_HAS_PASSWORD), nullptr
      ),
      g_uri_unref
  );
  if (!uri || g_uri_get_password(uri.get()) || g_uri_get_query(uri.get()) || g_uri_get_fragment(uri.get())) {
    return false;
  }
  const std::string_view scheme(g_uri_get_scheme(uri.get()));
  if (scheme == "file") {
    return location.isNative() && location.nativePath().has_value();
  }
  return scheme == "sftp" && g_uri_get_host(uri.get()) && *g_uri_get_host(uri.get());
}
