#pragma once

#include "browser/thumbnail_options.h"
#include "render/core/image_file_loader.h"

#include <gio/gio.h>

namespace files {
  std::expected<LoadedImageFile, std::string> renderThumbnailProvider(
      int sourceFd, const std::string& mime, int side, const ThumbnailOptions& options, GCancellable* cancellable
  );
}
