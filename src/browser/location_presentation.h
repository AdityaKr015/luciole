#pragma once

#include "fs/location.h"

#include <vector>

namespace files {
  struct Breadcrumb {
    Location location;
    std::string label;
  };

  // Pure URI ancestry; never queries directory contents or reconstructs targets from labels.
  std::vector<Breadcrumb> locationBreadcrumbs(const Location& location);
  std::string locationEditText(const Location& location);
  Location resolveLocationText(const std::string& text, const Location& base);
} // namespace files
