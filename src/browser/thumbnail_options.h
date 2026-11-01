#pragma once

namespace files {
  inline constexpr int kMaxPreviewImageSide = 2048;
  enum class ThumbnailPurpose { THUMBNAIL, PREVIEW };
  enum class ThumbnailPolicy { NEVER, LOCAL, ALL };
  struct ThumbnailOptions {
    ThumbnailPolicy policy = ThumbnailPolicy::LOCAL;
    int maxFileMiB = 64;
    int maxMegapixels = 32;
    int cacheMiB = 128;
    bool operator==(const ThumbnailOptions&) const = default;
  };
} // namespace files
