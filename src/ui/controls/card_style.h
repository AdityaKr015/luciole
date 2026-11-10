#pragma once

#include "ui/style.h"

namespace files {
  // Side-list metrics: panel padding, scroll viewport padding, row height, row padding and gap.
  inline constexpr float kSideListPadding = Style::spaceXs;
  inline constexpr float kSideListViewportPaddingV = Style::spaceMd;
  // Rows match the tab strip's tabs (controlHeightSm).
  inline constexpr float kSideListRowHeight = Style::controlHeightSm;
  inline constexpr float kSideListRowPaddingH = Style::spaceSm;
  inline constexpr float kSideListRowGap = Style::spaceSm;
} // namespace files
