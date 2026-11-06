#pragma once
#include "render/core/image_decoder.h"
namespace files {
  std::expected<DecodedRasterImage, std::string>
  decodePng(const std::uint8_t* data, std::size_t size, std::uint64_t maxPixels = 16U * 1024U * 1024U);
}
