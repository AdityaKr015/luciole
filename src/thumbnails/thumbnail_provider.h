#pragma once

#include "model/thumbnail_options.h"
#include "render/core/image_decoder.h"

#include <gio/gio.h>

namespace files {
  std::expected<LoadedImageFile, std::string> renderThumbnailProvider(
      int sourceFd, const std::string& mime, int side, const ThumbnailOptions& options, GCancellable* cancellable
  );
}
