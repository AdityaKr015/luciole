#pragma once

#include "ui/controls/flex.h"

#include <functional>
#include <memory>
#include <string>

namespace files {

  // Centered dialog root: Surface fill, 1px OutlineVariant border, radiusLg, spaceMd padding and gap.
  void applyDialogStyle(Flex& root);

  // Primary glyph, title (fontSizeTitle, SemiBold, one line), Ghost close button; then a separator.
  std::unique_ptr<Flex> dialogHeader(const std::string& title, const std::string& glyph, std::function<void()> close);

} // namespace files
