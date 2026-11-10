#pragma once

#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "ui/style.h"

namespace files {
  // Workspace cards (file panes, docked preview) share one frame.
  inline void applyCardStyle(Flex& card) {
    card.setFill(colorSpecFromRole(ColorRole::Surface));
    card.setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
    card.setRadius(Style::radiusXl);
  }
  // Side lists (places sidebar, settings navigation): a recessed SurfaceContainer panel without an outline.
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
