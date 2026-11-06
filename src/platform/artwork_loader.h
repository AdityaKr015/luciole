#pragma once
#include "render/core/image_decoder.h"
namespace files {
  std::expected<LoadedImageFile, std::string>
  loadArtwork(const std::string& path, int targetSize, bool centerSquareCrop);
}
