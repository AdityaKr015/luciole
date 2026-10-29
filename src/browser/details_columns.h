#pragma once

#include "browser/directory_options.h"

namespace files {
  struct DetailsColumns {
    bool showType = true;
    bool showSize = true;
    bool showModified = true;
    float nameWidth = 160.0F;
    float typeWidth = 144.0F;
    float sizeWidth = 110.0F;
    float modifiedWidth = 148.0F;
    float contentWidth = 602.0F;

    static DetailsColumns forWidth(
        float width, DetailsColumnVisibility visibility = {},
        DetailsColumnWidths requested = kDefaultDetailsColumnWidths
    ) {
      const auto metadata = static_cast<float>(
          (visibility.type ? requested[1] : 0)
          + (visibility.size ? requested[2] : 0)
          + (visibility.modified ? requested[3] : 0)
      );
      const float name =
          requested[0] == 0 ? std::clamp(width - 16.0F - metadata, 160.0F, 16384.0F) : static_cast<float>(requested[0]);
      return {
          .showType = visibility.type,
          .showSize = visibility.size,
          .showModified = visibility.modified,
          .nameWidth = name,
          .typeWidth = static_cast<float>(requested[1]) - 8.0F,
          .sizeWidth = static_cast<float>(requested[2]) - 8.0F,
          .modifiedWidth = static_cast<float>(requested[3]) - 8.0F,
          .contentWidth = 16.0F + name + metadata
      };
    }
    bool operator==(const DetailsColumns&) const = default;
  };
} // namespace files
