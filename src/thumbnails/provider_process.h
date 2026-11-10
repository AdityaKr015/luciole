#pragma once

#include <cstdint>
#include <expected>
#include <gio/gio.h>
#include <string>
#include <vector>

namespace luciole {
  // Worker-only execution with descriptor 3 as input and CPU, memory, time and file limits.
  // Optional stdout capture duplicates the supplied descriptor; the caller retains ownership.
  std::expected<void, std::string> runProviderProcess(
      std::vector<std::string> args, int input, GCancellable* cancellable, std::uint64_t fileBytes = 8U * 1024U * 1024U,
      unsigned maxCpus = 0, int output = -1
  );
} // namespace luciole
