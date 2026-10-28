#include "render/core/image_decoder.h"

#include <png.h>

std::expected<DecodedRasterImage, std::string> decodeRasterImage(const std::uint8_t* data, std::size_t size) {
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  if (data == nullptr || size == 0 || png_image_begin_read_from_memory(&image, data, size) == 0) {
    std::string error(image.message);
    png_image_free(&image);
    return std::unexpected(error.empty() ? "invalid PNG image" : error);
  }
  constexpr std::uint64_t kMaxPixels = 16U * 1024U * 1024U;
  if (image.width == 0 || image.height == 0 || static_cast<std::uint64_t>(image.width) * image.height > kMaxPixels) {
    png_image_free(&image);
    return std::unexpected("PNG dimensions exceed the decoded image budget");
  }
  image.format = PNG_FORMAT_RGBA;
  DecodedRasterImage decoded{
      .pixels = std::vector<std::uint8_t>(PNG_IMAGE_SIZE(image)),
      .width = static_cast<int>(image.width),
      .height = static_cast<int>(image.height),
  };
  if (png_image_finish_read(&image, nullptr, decoded.pixels.data(), 0, nullptr) == 0) {
    const std::string error(image.message);
    png_image_free(&image);
    return std::unexpected(error);
  }
  png_image_free(&image);
  return decoded;
}
