#pragma once

#include "render/core/image_decoder.h"

#include <expected>
#include <span>
#include <string>

namespace luciole {
  // Rasterizes an SVG file with librsvg, optionally center-cropped to a square, at most targetSize pixels per side.
  std::expected<LoadedImageFile, std::string>
  rasterizeSvg(const std::string& path, int targetSize, bool centerSquareCrop);
  // Decode captured bytes without a base URI; linked external resources stay unavailable.
  std::expected<LoadedImageFile, std::string> rasterizeSvg(
      std::span<const std::uint8_t> bytes, int targetSize, bool centerSquareCrop, std::uint64_t maxSourcePixels
  );
} // namespace luciole
