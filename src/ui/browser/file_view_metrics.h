#pragma once

#include "model/directory_options.h"

#include <algorithm>

namespace files {
  struct FileViewMetrics {
    float inset, iconSize, nameTop, captionTop, parentTop, height, minWidth, gap;
  };

  // Shared by cell layout, viewport allocation and pointer hit testing.
  inline FileViewMetrics gridMetrics(float iconSize, BrowsePresentation presentation, bool context) {
    const bool comfortable = presentation.density == ViewDensity::Comfortable;
    const float inset = comfortable ? 12.0F : 8.0F;
    const float nameTop = inset + iconSize + (comfortable ? 8.0F : 4.0F);
    const float captionTop = nameTop + 40.0F;
    const float parentTop = captionTop + 18.0F * presentation.captions.count();
    return {
        inset,
        iconSize,
        nameTop,
        captionTop,
        parentTop,
        parentTop + (context ? 20.0F : 0.0F) + (comfortable ? 8.0F : 0.0F),
        std::max(88.0F, iconSize + (comfortable ? 72.0F : 56.0F)),
        comfortable ? 8.0F : 4.0F
    };
  }
  inline float fileRowHeight(BrowsePresentation presentation, bool context) {
    return (presentation.density == ViewDensity::Comfortable ? 44.0F : 36.0F) + (context ? 16.0F : 0.0F);
  }
  inline float fileRowIconSize(BrowsePresentation presentation) {
    return presentation.density == ViewDensity::Comfortable ? 28.0F : 24.0F;
  }
} // namespace files
