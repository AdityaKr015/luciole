#pragma once

#include "operations/archive_input.h"

namespace files::archive_work {
  // Decrypt one regular member to an already-open private file. Native metadata
  // supplies its literal name and expected size; no CLI listing is parsed.
  void extractEncryptedSevenZip(
      Input& input, int output, const ArchiveEntry& entry, Work& work, const Passwords& passwords, std::uint64_t& total,
      std::uint32_t index = 0
  );
} // namespace files::archive_work
