#pragma once

#include "render/core/image_file_loader.h"

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace files {
  // Private decoder protocol. Input is inherited descriptor 3; output must not already exist.
  int runThumbnailWorker(std::span<const std::string_view> arguments);
  // Validate the bounded private RGBA result. Shared on-disk thumbnails remain PNG.
  std::expected<LoadedImageFile, std::string> readThumbnailWorkerResult(std::vector<std::uint8_t> bytes, int maxSide);
} // namespace files
