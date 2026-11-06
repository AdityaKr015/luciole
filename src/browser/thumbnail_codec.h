#pragma once

#include "render/core/image_decoder.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <map>
#include <span>

namespace files {
  struct ThumbnailPng {
    LoadedImageFile image;
    std::map<std::string, std::string> text;
  };
  std::expected<LoadedImageFile, std::string> decodeThumbnail(
      std::span<const std::uint8_t> bytes, int target, std::uint64_t maxPixels, GCancellable* cancellable,
      std::string_view contentType = {}
  );
  // Some system loaders return full-size pixels despite a requested decode size.
  std::expected<LoadedImageFile, std::string>
  fitThumbnailPixels(GdkPixbuf* pixbuf, int target, std::uint64_t maxPixels);
  std::expected<ThumbnailPng, std::string> readThumbnailPng(std::span<const std::uint8_t> bytes, int maxSide);
  std::expected<std::vector<std::uint8_t>, std::string> writeThumbnailPng(const ThumbnailPng& thumbnail);
} // namespace files
