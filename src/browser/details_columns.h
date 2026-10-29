#pragma once

namespace files {
  struct DetailsColumns {
    bool showType = true;
    bool compactDate = false;
    float sizeWidth = 110.0F;
    float modifiedWidth = 148.0F;
    static constexpr float typeWidth = 64.0F;

    static DetailsColumns forWidth(float width) {
      const bool compact = width < 560.0F;
      return {width >= 640.0F, compact, compact ? 80.0F : 110.0F, compact ? 90.0F : 148.0F};
    }

    bool operator==(const DetailsColumns&) const = default;
  };
} // namespace files
