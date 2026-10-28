#pragma once

#include <cstdint>
#include <string>

namespace files {
  enum class SortField : std::uint8_t { NAME, SIZE, MODIFIED };
  struct SortOptions {
    SortField field = SortField::NAME;
    bool descending = false;
    bool directoriesFirst = true;
    bool operator==(const SortOptions&) const = default;
  };

  struct FilterOptions {
    bool showHidden = false;
    std::string text; // Literal substring of the display name, folded and normalized.
    bool operator==(const FilterOptions&) const = default;
  };
} // namespace files
