#pragma once

#include "render/core/image_decoder.h"

#include <expected>
#include <string>

namespace files {
  // Rasterizes an SVG file with librsvg, optionally center-cropped to a square, at most targetSize pixels per side.
  std::expected<LoadedImageFile, std::string>
  rasterizeSvg(const std::string& path, int targetSize, bool centerSquareCrop);
} // namespace files
