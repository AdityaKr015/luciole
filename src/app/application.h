#pragma once

#include <optional>
#include <string>

namespace files {
  int runApplication(const std::optional<std::string>& initialLocation = std::nullopt);
}
