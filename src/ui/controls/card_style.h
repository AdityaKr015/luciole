#pragma once

#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

namespace files {
  // Settings navigation: a SurfaceContainer panel (the window chrome colour) inside the Surface dialog.
  inline void applySideListStyle(Flex& panel) {
    panel.setFill(colorSpecFromRole(ColorRole::SurfaceContainer));
    panel.setRadius(Style::radiusXl);
  }
  // Side-list metrics: panel padding, scroll viewport padding, row height, row padding and gap.
  inline constexpr float kSideListPadding = Style::spaceXs;
  inline constexpr float kSideListViewportPaddingH = Style::spaceSm;
  inline constexpr float kSideListViewportPaddingV = Style::spaceMd;
  // Rows match the tab strip's tabs (controlHeightSm).
  inline constexpr float kSideListRowHeight = Style::controlHeightSm;
  inline constexpr float kSideListRowPaddingH = Style::spaceSm;
  inline constexpr float kSideListRowGap = Style::spaceSm;
} // namespace files
