#pragma once

#include "fs/gio_ptr.h"

#include <glib.h>
#include <string>

namespace files {
  inline std::string foldedDisplayText(const std::string& text) {
    const GString valid(g_utf8_make_valid(text.c_str(), static_cast<gssize>(text.size())));
    const GString lower(g_utf8_casefold(valid.get(), -1));
    const GString normalized(g_utf8_normalize(lower.get(), -1, G_NORMALIZE_DEFAULT_COMPOSE));
    return normalized.get();
  }
} // namespace files
