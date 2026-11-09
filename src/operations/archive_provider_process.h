#pragma once

#include "operations/archive_input.h"

namespace files::archive_work {
  // Source fd 3 and password stdin are private to the bounded child. argv is NUL-terminated.
  int runArchiveProvider(
      Input& input, int output, std::uint64_t maximum, Work& work, std::string_view password,
      std::span<const char* const> argv
  );
} // namespace files::archive_work
