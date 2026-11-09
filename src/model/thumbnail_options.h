#pragma once

namespace files {
  inline constexpr int kMaxPreviewImageSide = 2048;
  enum class ThumbnailPurpose { Thumbnail, Preview };
  enum class ThumbnailPolicy { Never, Local, All };
  struct ThumbnailOptions {
    ThumbnailPolicy policy = ThumbnailPolicy::Local;
    int maxFileMiB = 64;
    int maxMegapixels = 32;
    int cacheMiB = 128;
    bool operator==(const ThumbnailOptions&) const = default;
  };
} // namespace files
