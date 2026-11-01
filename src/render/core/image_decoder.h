#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

struct DecodedRasterImage {
  std::vector<std::uint8_t> pixels;
  int width = 0;
  int height = 0;
};

[[nodiscard]] std::expected<DecodedRasterImage, std::string>
decodeRasterImage(const std::uint8_t* data, std::size_t size, std::uint64_t maxPixels = 16U * 1024U * 1024U);
