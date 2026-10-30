#pragma once

#include <span>
#include <string_view>

namespace files {
  // Private decoder protocol. Input is inherited descriptor 3; output must not already exist.
  int runThumbnailWorker(std::span<const std::string_view> arguments);
} // namespace files
