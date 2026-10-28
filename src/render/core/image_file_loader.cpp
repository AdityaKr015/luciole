#include "render/core/image_file_loader.h"

#include "render/core/image_decoder.h"

#include <algorithm>
#include <cairo.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <librsvg/rsvg.h>

namespace {
  // Convert Cairo's native-endian premultiplied pixels back to straight RGBA.
  void copyPixels(cairo_surface_t* surface, LoadedImageFile& image) {
    cairo_surface_flush(surface);
    const auto* data = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < image.height; ++y) {
      const auto* row = reinterpret_cast<const std::uint32_t*>(data + y * stride);
      for (int x = 0; x < image.width; ++x) {
        const auto pixel = row[x];
        const auto alpha = (pixel >> 24U) & 255U;
        const auto index = (static_cast<std::size_t>(y) * image.width + x) * 4U;
        for (std::size_t channel = 0; channel < 3; ++channel) {
          const auto value = (pixel >> (16U - static_cast<unsigned>(channel) * 8U)) & 255U;
          image.rgba[index + channel] =
              static_cast<std::uint8_t>(alpha == 0 ? 0 : std::min(255U, (value * 255U + alpha / 2U) / alpha));
        }
        image.rgba[index + 3] = static_cast<std::uint8_t>(alpha);
      }
    }
  }
} // namespace

std::expected<LoadedImageFile, std::string>
loadImageFile(const std::string& path, int targetSize, bool centerSquareCrop) {
  constexpr std::size_t kMaxEncodedBytes = 64U * 1024U * 1024U;
  constexpr double kMaxPixels = 16.0 * 1024.0 * 1024.0;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return std::unexpected("image path is not a regular file");
  }
  const auto length = std::filesystem::file_size(path, ec);
  if (ec || length == 0 || length > kMaxEncodedBytes) {
    return std::unexpected("image file exceeds the encoded size budget");
  }
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
  if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
    return std::unexpected("cannot read image file");
  }

  RsvgHandle* svg = nullptr;
  cairo_surface_t* raster = nullptr;
  double sourceWidth = 0;
  double sourceHeight = 0;
  const auto extension = std::filesystem::path(path).extension();
  if (extension == ".svg" || extension == ".SVG") {
    GError* error = nullptr;
    svg = rsvg_handle_new_from_data(bytes.data(), bytes.size(), &error);
    if (svg == nullptr) {
      const std::string message = error != nullptr ? error->message : "invalid SVG";
      if (error != nullptr) {
        g_error_free(error);
      }
      return std::unexpected(message);
    }
    if (!rsvg_handle_get_intrinsic_size_in_pixels(svg, &sourceWidth, &sourceHeight)) {
      gboolean hasWidth = FALSE, hasHeight = FALSE, hasViewbox = FALSE;
      RsvgLength width{}, height{};
      RsvgRectangle viewbox{};
      rsvg_handle_get_intrinsic_dimensions(svg, &hasWidth, &width, &hasHeight, &height, &hasViewbox, &viewbox);
      sourceWidth = hasViewbox ? viewbox.width : 256.0;
      sourceHeight = hasViewbox ? viewbox.height : 256.0;
    }
  } else {
    auto decoded = decodeRasterImage(bytes.data(), bytes.size());
    if (!decoded) {
      return std::unexpected(decoded.error());
    }
    sourceWidth = decoded->width;
    sourceHeight = decoded->height;
    raster = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, decoded->width, decoded->height);
    if (cairo_surface_status(raster) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(raster);
      return std::unexpected("cannot allocate image surface");
    }
    auto* data = cairo_image_surface_get_data(raster);
    const int stride = cairo_image_surface_get_stride(raster);
    for (int y = 0; y < decoded->height; ++y) {
      auto* row = reinterpret_cast<std::uint32_t*>(data + y * stride);
      for (int x = 0; x < decoded->width; ++x) {
        const auto index = (static_cast<std::size_t>(y) * decoded->width + x) * 4U;
        const std::uint32_t alpha = decoded->pixels[index + 3];
        row[x] = (alpha << 24U)
            | (((decoded->pixels[index] * alpha + 127U) / 255U) << 16U)
            | (((decoded->pixels[index + 1] * alpha + 127U) / 255U) << 8U)
            | ((decoded->pixels[index + 2] * alpha + 127U) / 255U);
      }
    }
    cairo_surface_mark_dirty(raster);
  }

  const auto release = [&] {
    if (svg != nullptr) {
      g_object_unref(svg);
    }
    if (raster != nullptr) {
      cairo_surface_destroy(raster);
    }
  };
  if (!std::isfinite(sourceWidth)
      || !std::isfinite(sourceHeight)
      || sourceWidth <= 0
      || sourceHeight <= 0
      || sourceWidth > 32768
      || sourceHeight > 32768) {
    release();
    return std::unexpected("invalid image dimensions");
  }
  const double cropWidth = centerSquareCrop ? std::min(sourceWidth, sourceHeight) : sourceWidth;
  const double cropHeight = centerSquareCrop ? cropWidth : sourceHeight;
  const double scale =
      targetSize > 0 ? static_cast<double>(std::min(targetSize, 4096)) / std::max(cropWidth, cropHeight) : 1.0;
  const int width = std::max(1, static_cast<int>(std::round(cropWidth * scale)));
  const int height = std::max(1, static_cast<int>(std::round(cropHeight * scale)));
  if (static_cast<double>(width) * height > kMaxPixels) {
    release();
    return std::unexpected("decoded image exceeds its budget");
  }
  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    release();
    return std::unexpected("cannot allocate resized image");
  }
  cairo_t* cr = cairo_create(surface);
  cairo_scale(cr, static_cast<double>(width) / cropWidth, static_cast<double>(height) / cropHeight);
  cairo_translate(cr, -(sourceWidth - cropWidth) / 2.0, -(sourceHeight - cropHeight) / 2.0);
  bool success = true;
  if (svg != nullptr) {
    const RsvgRectangle viewport{.x = 0, .y = 0, .width = sourceWidth, .height = sourceHeight};
    success = rsvg_handle_render_document(svg, cr, &viewport, nullptr);
  } else {
    cairo_set_source_surface(cr, raster, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint(cr);
  }
  success = success && cairo_status(cr) == CAIRO_STATUS_SUCCESS;
  cairo_destroy(cr);
  LoadedImageFile image{
      .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4U),
      .width = width,
      .height = height,
      .sourceWidth = static_cast<int>(sourceWidth),
      .sourceHeight = static_cast<int>(sourceHeight),
  };
  if (success) {
    copyPixels(surface, image);
  }
  cairo_surface_destroy(surface);
  release();
  if (!success) {
    return std::unexpected("image rasterization failed");
  }
  return image;
}
