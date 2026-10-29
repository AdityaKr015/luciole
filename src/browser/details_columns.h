#pragma once

#include "browser/directory_options.h"

namespace files {
  struct DetailsColumns {
    bool showType = true;
    bool showSize = true;
    bool showModified = true;
    bool compactDate = false;
    float sizeWidth = 110.0F;
    float modifiedWidth = 148.0F;
    static constexpr float typeWidth = 144.0F;

    static DetailsColumns forWidth(float width, DetailsColumnVisibility visibility = {}) {
      const bool compact = width < 560.0F;
      const float typeThreshold = 640.0F - (visibility.size ? 0.0F : 118.0F) - (visibility.modified ? 0.0F : 156.0F);
      return {
          .showType = visibility.type && width >= typeThreshold,
          .showSize = visibility.size,
          .showModified = visibility.modified,
          .compactDate = compact,
          .sizeWidth = compact ? 80.0F : 110.0F,
          .modifiedWidth = compact ? 90.0F : 148.0F
      };
    }

    bool operator==(const DetailsColumns&) const = default;
  };
} // namespace files
