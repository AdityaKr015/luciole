#include "browser/thumbnail_codec.h"

#include "fs/gio_ptr.h"
#include "render/core/image_orientation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <png.h>
#include <webp/decode.h>

namespace {
  std::uint32_t read32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24)
        | (static_cast<std::uint32_t>(p[1]) << 16)
        | (static_cast<std::uint32_t>(p[2]) << 8)
        | p[3];
  }
  std::uint32_t pngCrc(std::span<const std::uint8_t> bytes) {
    static constexpr auto table = [] {
      std::array<std::uint32_t, 256> values{};
      for (std::uint32_t value = 0; value < values.size(); ++value) {
        auto crc = value;
        for (int bit = 0; bit < 8; ++bit)
          crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        values[value] = crc;
      }
      return values;
    }();
    std::uint32_t crc = 0xffffffffU;
    for (auto byte : bytes)
      crc = table[(crc ^ byte) & 255U] ^ (crc >> 8);
    return crc ^ 0xffffffffU;
  }
  struct DecodeSize {
    int target;
    std::uint64_t maxPixels;
    bool rejected = false;
    int width = 0, height = 0;
  };
  void sizePrepared(GdkPixbufLoader* loader, int width, int height, gpointer data) {
    auto& state = *static_cast<DecodeSize*>(data);
    state.width = width;
    state.height = height;
    state.rejected = width <= 0 || height <= 0 || static_cast<std::uint64_t>(width) * height > state.maxPixels;
    const double ratio =
        state.rejected ? 0 : std::min(1.0, static_cast<double>(state.target) / std::max(width, height));
    gdk_pixbuf_loader_set_size(
        loader, std::max(1, static_cast<int>(std::round(width * ratio))),
        std::max(1, static_cast<int>(std::round(height * ratio)))
    );
  }
} // namespace

std::expected<LoadedImageFile, std::string> files::decodeThumbnail(
    std::span<const std::uint8_t> bytes, int target, std::uint64_t maxPixels, GCancellable* cancellable
) {
  if (bytes.empty() || target < 1 || target > 1024)
    return std::unexpected("invalid thumbnail input");
  const auto starts = [&](std::string_view signature) {
    return bytes.size() >= signature.size() && std::memcmp(bytes.data(), signature.data(), signature.size()) == 0;
  };
  const bool isWebp = bytes.size() >= 12 && starts("RIFF") && std::memcmp(bytes.data() + 8, "WEBP", 4) == 0;
  if (isWebp) {
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)
        || WebPGetFeatures(bytes.data(), bytes.size(), &config.input) != VP8_STATUS_OK
        || config.input.width < 1
        || config.input.height < 1
        || static_cast<std::uint64_t>(config.input.width) * config.input.height > maxPixels)
      return std::unexpected("invalid or oversized WebP");
    const double ratio = std::min(1.0, static_cast<double>(target) / std::max(config.input.width, config.input.height));
    const int width = std::max(1, static_cast<int>(std::round(config.input.width * ratio))),
              height = std::max(1, static_cast<int>(std::round(config.input.height * ratio)));
    LoadedImageFile image{
        .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4),
        .width = width,
        .height = height,
        .sourceWidth = config.input.width,
        .sourceHeight = config.input.height
    };
    config.options.use_scaling = width != config.input.width || height != config.input.height;
    config.options.scaled_width = width;
    config.options.scaled_height = height;
    config.output.colorspace = MODE_RGBA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = image.rgba.data();
    config.output.u.RGBA.stride = width * 4;
    config.output.u.RGBA.size = image.rgba.size();
    const auto status = WebPDecode(bytes.data(), bytes.size(), &config);
    WebPFreeDecBuffer(&config.output);
    if (status != VP8_STATUS_OK || g_cancellable_is_cancelled(cancellable))
      return std::unexpected("unsupported or cancelled WebP");
    applyImageOrientation(image.rgba, image.width, image.height, exifOrientation(bytes.data(), bytes.size()));
    return image;
  }
  // Only binary raster formats reach GdkPixbuf. In particular, SVG/XML never resolves resources.
  const bool raster = starts(std::string_view("\x89PNG\r\n\x1a\n", 8))
      || starts(std::string_view("\xff\xd8", 2))
      || starts("GIF87a")
      || starts("GIF89a")
      || starts("BM")
      || starts(std::string_view("II*\0", 4))
      || starts(std::string_view("MM\0*", 4))
      || starts(std::string_view("\0\0\1\0", 4))
      || starts("8BPS")
      || (bytes.size() >= 3 && bytes[0] == 'P' && bytes[1] >= '1' && bytes[1] <= '6' && g_ascii_isspace(bytes[2]));
  if (!raster)
    return std::unexpected("no supported raster decoder");
  // SVG external-resource resolution is left to a separately verified provider.
  const GioPtr<GdkPixbufLoader> loader(gdk_pixbuf_loader_new());
  DecodeSize state{target, maxPixels};
  g_signal_connect(loader.get(), "size-prepared", G_CALLBACK(sizePrepared), &state);
  GError* error = nullptr;
  bool ok = true;
  for (std::size_t offset = 0; offset < bytes.size(); offset += 65536) {
    if (g_cancellable_is_cancelled(cancellable) || state.rejected) {
      ok = false;
      break;
    }
    const auto count = std::min<std::size_t>(65536, bytes.size() - offset);
    if (!gdk_pixbuf_loader_write(loader.get(), bytes.data() + offset, count, &error)) {
      ok = false;
      break;
    }
    if (auto* format = gdk_pixbuf_loader_get_format(loader.get())) {
      const GString name(gdk_pixbuf_format_get_name(format));
      if (name && std::strcmp(name.get(), "svg") == 0) {
        ok = false;
        break;
      }
    }
  }
  if (error)
    g_clear_error(&error);
  if (!gdk_pixbuf_loader_close(loader.get(), &error))
    ok = false;
  if (error)
    g_clear_error(&error);
  auto* pixbuf = gdk_pixbuf_loader_get_pixbuf(loader.get());
  if (!ok || state.rejected || !pixbuf || g_cancellable_is_cancelled(cancellable))
    return std::unexpected("unsupported, cancelled or oversized image");
  const int width = gdk_pixbuf_get_width(pixbuf), height = gdk_pixbuf_get_height(pixbuf);
  if (width < 1 || height < 1 || width > target || height > target || gdk_pixbuf_get_bits_per_sample(pixbuf) != 8)
    return std::unexpected("decoder ignored thumbnail bounds");
  LoadedImageFile image{
      .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4),
      .width = width,
      .height = height,
      .sourceWidth = state.width,
      .sourceHeight = state.height
  };
  const int channels = gdk_pixbuf_get_n_channels(pixbuf), stride = gdk_pixbuf_get_rowstride(pixbuf);
  if (channels != 3 && channels != 4)
    return std::unexpected("unsupported image channels");
  const auto* pixels = gdk_pixbuf_read_pixels(pixbuf);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      const auto source = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * channels;
      const auto dest = (static_cast<std::size_t>(y) * width + x) * 4;
      for (int c = 0; c < 3; ++c)
        image.rgba[dest + c] = pixels[source + c];
      image.rgba[dest + 3] = channels == 4 ? pixels[source + 3] : 255;
    }
  applyImageOrientation(image.rgba, image.width, image.height, exifOrientation(bytes.data(), bytes.size()));
  return image;
}

std::expected<files::ThumbnailPng, std::string>
files::readThumbnailPng(std::span<const std::uint8_t> bytes, int maxSide) {
  constexpr std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
  if (maxSide < 1
      || maxSide > 1024
      || bytes.size() < 45
      || bytes.size() > 8U * 1024U * 1024U
      || !std::ranges::equal(bytes.first(8), signature))
    return std::unexpected("invalid thumbnail PNG");
  ThumbnailPng result;
  bool ended = false, filter = false;
  const auto pixelChunk = [](std::string_view type, std::uint32_t length) {
    return type == "IHDR"
        || type == "IDAT"
        || type == "PLTE"
        || type == "tRNS"
        || type == "IEND"
        || (type == "gAMA" && length == 4)
        || (type == "sRGB" && length == 1)
        || (type == "cHRM" && length == 32);
  };
  for (std::size_t offset = 8; offset + 12 <= bytes.size();) {
    const auto* p = bytes.data() + offset;
    const auto length = read32(p);
    if (length > bytes.size() - offset - 12
        || read32(p + 8 + length) != pngCrc(bytes.subspan(offset + 4, static_cast<std::size_t>(length) + 4)))
      return std::unexpected("invalid PNG chunk");
    const std::string_view type(reinterpret_cast<const char*>(p + 4), 4);
    if (offset == 8 && (type != "IHDR" || length != 13))
      return std::unexpected("missing PNG header");
    if (type == "tEXt") {
      if (length > 131072)
        return std::unexpected("oversized PNG metadata");
      const auto* begin = reinterpret_cast<const char*>(p + 8);
      const auto* split = static_cast<const char*>(std::memchr(begin, 0, length));
      if (!split || split == begin || split - begin > 79 || result.text.size() >= 64)
        return std::unexpected("invalid PNG metadata");
      if (!result.text.emplace(std::string(begin, split), std::string(split + 1, begin + length)).second)
        return std::unexpected("duplicate PNG metadata");
    } else if (!pixelChunk(type, length)) {
      if ((p[4] & 32U) == 0)
        return std::unexpected("unsupported critical PNG chunk");
      filter = true;
    }
    offset += static_cast<std::size_t>(length) + 12;
    if (type == "IEND") {
      ended = length == 0 && offset == bytes.size();
      break;
    }
  }
  if (!ended)
    return std::unexpected("missing PNG end");
  // Ordinary shared-cache PNGs need no copy. Strip potentially compressed ancillary metadata only when present.
  std::vector<std::uint8_t> pixels;
  if (filter) {
    pixels.assign(signature.begin(), signature.end());
    pixels.reserve(bytes.size());
    for (std::size_t offset = 8; offset + 12 <= bytes.size();) {
      const auto length = read32(bytes.data() + offset);
      const std::string_view type(reinterpret_cast<const char*>(bytes.data() + offset + 4), 4);
      const auto end = offset + length + 12;
      if (pixelChunk(type, length))
        pixels.insert(
            pixels.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes.begin() + static_cast<std::ptrdiff_t>(end)
        );
      offset = end;
    }
    bytes = pixels;
  }
  png_image header{};
  header.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&header, bytes.data(), bytes.size())) {
    png_image_free(&header);
    return std::unexpected("invalid thumbnail PNG");
  }
  const bool valid = header.width > 0
      && header.height > 0
      && header.width <= static_cast<unsigned>(maxSide)
      && header.height <= static_cast<unsigned>(maxSide);
  header.format = PNG_FORMAT_RGBA;
  if (valid)
    result.image = {
        .rgba = std::vector<std::uint8_t>(PNG_IMAGE_SIZE(header)),
        .width = static_cast<int>(header.width),
        .height = static_cast<int>(header.height)
    };
  if (!valid || !png_image_finish_read(&header, nullptr, result.image.rgba.data(), 0, nullptr)) {
    png_image_free(&header);
    return std::unexpected("oversized or corrupt thumbnail PNG");
  }
  png_image_free(&header);
  return result;
}

std::expected<std::vector<std::uint8_t>, std::string> files::writeThumbnailPng(const ThumbnailPng& thumbnail) {
  const auto& image = thumbnail.image;
  if (image.width < 1
      || image.height < 1
      || image.width > 1024
      || image.height > 1024
      || image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4)
    return std::unexpected("invalid thumbnail pixels");
  png_image output{};
  output.version = PNG_IMAGE_VERSION;
  output.width = image.width;
  output.height = image.height;
  output.format = PNG_FORMAT_RGBA;
  // The documented bound avoids compressing the complete image once just to measure its size.
  png_alloc_size_t size = PNG_IMAGE_PNG_SIZE_MAX(output);
  std::vector<std::uint8_t> encoded(size);
  if (!png_image_write_to_memory(&output, encoded.data(), &size, 0, image.rgba.data(), 0, nullptr)) {
    png_image_free(&output);
    return std::unexpected("cannot encode thumbnail");
  }
  png_image_free(&output);
  encoded.resize(size);
  // Insert required standard tEXt keys directly before IEND, retaining ordinary RGBA PNG output.
  std::vector<std::uint8_t> metadata;
  const auto append32 = [&](std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
      metadata.push_back(static_cast<std::uint8_t>(value >> shift));
  };
  for (const auto& [key, value] : thumbnail.text) {
    if (key.empty() || key.size() > 79 || value.size() > 131072 || key.contains('\0') || value.contains('\0'))
      continue;
    append32(static_cast<std::uint32_t>(key.size() + 1 + value.size()));
    const auto start = metadata.size();
    for (char c : std::string_view("tEXt"))
      metadata.push_back(static_cast<std::uint8_t>(c));
    metadata.insert(metadata.end(), key.begin(), key.end());
    metadata.push_back(0);
    metadata.insert(metadata.end(), value.begin(), value.end());
    // PNG uses the standard CRC-32 polynomial; no zlib API is needed for this small metadata block.
    append32(pngCrc(std::span<const std::uint8_t>(metadata).subspan(start)));
  }
  encoded.insert(encoded.end() - 12, metadata.begin(), metadata.end());
  return encoded;
}
