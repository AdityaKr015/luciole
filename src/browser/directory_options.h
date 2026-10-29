#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace files {
  inline constexpr std::array<int, 6> kGridIconSizes{32, 48, 64, 80, 96, 128};
  inline constexpr int kDefaultGridIconSize = 48;
  inline constexpr int kMinSidebarWidth = 160;
  inline constexpr int kMaxSidebarWidth = 400;
  inline constexpr int kDefaultSidebarWidth = 206;
  enum class ViewMode : std::uint8_t { LIST, GRID };
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
