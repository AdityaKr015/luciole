#pragma once

#include <span>
#include <string_view>

namespace files {

  int runCli(std::span<const std::string_view> args);

} // namespace files
