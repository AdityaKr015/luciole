#include "platform/pixbuf_image_decoder.h"

#include "browser/thumbnail_codec.h"
#include "platform/artwork_loader.h"
#include "render/core/image_decoder.h"
#include "render/core/image_resize.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>

namespace {
  constexpr std::size_t kMaxEncoded = 64U * 1024U * 1024U;
  constexpr std::uint64_t kMaxPixels = 16U * 1024U * 1024U;
  constexpr int kMaxSide = 8192;

  class PixbufImageDecoder final : public ImageDecoder {
  public:
    std::expected<DecodedRasterImage, std::string> decode(const std::uint8_t* data, std::size_t size) override {
      if (!data || !size || size > kMaxEncoded) {
        return std::unexpected("encoded image exceeds its budget");
      }
      auto image = files::decodeThumbnail({data, size}, kMaxSide, kMaxPixels, nullptr);
      if (!image) {
        return std::unexpected(image.error());
      }
      if (image->width != image->sourceWidth || image->height != image->sourceHeight) {
        return std::unexpected("decoded image exceeds its side limit");
      }
      return DecodedRasterImage{.pixels = std::move(image->rgba), .width = image->width, .height = image->height};
    }

    std::expected<LoadedImageFile, std::string>
    loadFile(const std::string& path, int targetSize, bool centerSquareCrop) override {
      std::vector<std::uint8_t> bytes;
      if (path.starts_with("data:")) {
        const auto comma = path.find(',');
        if (comma == std::string::npos
            || !std::string_view(path).substr(0, comma).ends_with(";base64")
            || path.size() - comma - 1 > kMaxEncoded) {
          return std::unexpected("invalid or oversized image data URI");
        }
        const auto payload = std::string_view(path).substr(comma + 1);
        if (payload.size() % 4 != 0 || !std::ranges::all_of(payload, [](unsigned char c) {
              return (c >= 'A' && c <= 'Z')
                  || (c >= 'a' && c <= 'z')
                  || (c >= '0' && c <= '9')
                  || c == '+'
                  || c == '/'
                  || c == '=';
            })) {
          return std::unexpected("invalid image base64 payload");
        }
        gsize length = 0;
        const std::unique_ptr<guchar, decltype(&g_free)> decoded(
            g_base64_decode(path.c_str() + comma + 1, &length), &g_free
        );
        bytes.assign(decoded.get(), decoded.get() + length);
      } else {
        const auto extension = std::filesystem::path(path).extension();
        if (extension == ".svg" || extension == ".SVG") {
          return files::loadArtwork(path, targetSize, centerSquareCrop);
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
          return std::unexpected("image path is not a regular file");
        }
        const auto length = std::filesystem::file_size(path, error);
        if (error || length == 0 || length > kMaxEncoded) {
          return std::unexpected("encoded image exceeds its budget");
        }
        bytes.resize(static_cast<std::size_t>(length));
        std::ifstream stream(path, std::ios::binary);
        if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
          return std::unexpected("cannot read image file");
        }
      }
      auto image = files::decodeThumbnail(
          bytes, targetSize > 0 && !centerSquareCrop ? std::min(targetSize, kMaxSide) : kMaxSide, kMaxPixels, nullptr
      );
      if (!image || !centerSquareCrop) {
        return image;
      }
      const int side = std::min(image->width, image->height);
      const int left = (image->width - side) / 2, top = (image->height - side) / 2;
      std::vector<std::uint8_t> cropped(static_cast<std::size_t>(side) * side * 4);
      for (int y = 0; y < side; ++y) {
        std::copy_n(
            image->rgba.data() + (static_cast<std::size_t>(top + y) * image->width + left) * 4,
            static_cast<std::size_t>(side) * 4, cropped.data() + static_cast<std::size_t>(y) * side * 4
        );
      }
      const int outputSide = targetSize > 0 ? std::min(side, targetSize) : side;
      image->rgba = outputSide == side
          ? std::move(cropped)
          : resizeToRgba(cropped.data(), side, side, ResizeSourceLayout::Rgba, outputSide, outputSide);
      if (image->rgba.empty()) {
        return std::unexpected("cannot resize cropped image");
      }
      image->width = image->height = outputSide;
      return image;
    }
  };
} // namespace

void files::installPixbufImageDecoder() { setImageDecoder(std::make_unique<PixbufImageDecoder>()); }
