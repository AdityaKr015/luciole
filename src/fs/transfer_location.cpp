#include "fs/gio_interop.h"
#include "fs/transfer_types.h"

#include <algorithm>
#include <string_view>

namespace {
  using Uri = std::unique_ptr<GUri, decltype(&g_uri_unref)>;
  Uri parsed(const files::Location& location) {
    return Uri(g_uri_parse(location.uri().c_str(), G_URI_FLAGS_NONE, nullptr), g_uri_unref);
  }
  std::string folded(const char* value) {
    if (!value || !g_utf8_validate(value, -1, nullptr)) {
      return {};
    }
    const files::GString text(g_utf8_casefold(value, -1));
    return text.get();
  }
  int smbPort(GUri* uri) {
    const auto value = g_uri_get_port(uri);
    return value < 0 ? 445 : value;
  }
  std::optional<std::string> smbPath(const files::Location& source, const files::Location& target) {
    const auto a = parsed(source), b = parsed(target);
    if (!a
        || !b
        || std::string_view(g_uri_get_scheme(a.get())) != "smb"
        || std::string_view(g_uri_get_scheme(b.get())) != "smb"
        || folded(g_uri_get_host(a.get())) != folded(g_uri_get_host(b.get()))
        || smbPort(a.get()) != smbPort(b.get())) {
      return {};
    }
    return folded(g_uri_get_path(b.get()));
  }
} // namespace

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
  return (scheme == "sftp" || scheme == "smb" || scheme == "dav" || scheme == "davs")
      && g_uri_get_host(uri.get())
      && *g_uri_get_host(uri.get());
}

// SMB names are commonly case-insensitive. Conservatively treat case-folded aliases
// as the same operation target even when the GFile URI preserves their spelling.
bool files::transferLocationsEqual(const Location& source, const Location& target) {
  if (source == target) {
    return true;
  }
  const auto path = smbPath(source, target);
  if (!path) {
    return false;
  }
  const auto a = parsed(source);
  return folded(g_uri_get_path(a.get())) == *path;
}
bool files::transferLocationInside(const Location& child, const Location& parent) {
  if (child.isInside(parent)) {
    return true;
  }
  const auto path = smbPath(parent, child);
  if (!path) {
    return false;
  }
  const auto a = parsed(parent);
  auto prefix = folded(g_uri_get_path(a.get()));
  if (!prefix.ends_with('/')) {
    prefix += '/';
  }
  return path->starts_with(prefix);
}

bool files::transferAffectsLocation(
    const std::vector<TransferResult>& results, const Location& location, bool recursive
) {
  const auto affected = [&](const Location& item) {
    const auto parent = item.parent();
    return transferLocationsEqual(item, location)
        || transferLocationInside(location, item)
        || (parent && transferLocationsEqual(*parent, location))
        || (recursive && transferLocationInside(item, location));
  };
  return std::ranges::any_of(results, [&](const auto& result) {
    // Root locations cover their merge children too. Never scan the potentially
    // 100000 child outcomes separately on the UI thread.
    return affected(result.source) || (result.destination && affected(*result.destination));
  });
}
