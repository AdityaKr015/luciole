#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace files {
  std::optional<std::string> filenameProblem(const std::string& name);
  std::size_t filenameStemLength(const std::string& name, bool directory);
} // namespace files
