#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace files {
  inline constexpr std::array<int, 9> kGridIconSizes{32, 48, 64, 80, 96, 128, 160, 192, 256};
  inline constexpr int kDefaultGridIconSize = 48;
  inline constexpr int kMinSidebarWidth = 160;
  inline constexpr int kMaxSidebarWidth = 400;
  inline constexpr int kDefaultSidebarWidth = 206;
  enum class ViewMode : std::uint8_t { LIST, GRID };
  struct DetailsColumnVisibility {
    bool type = true;
    bool size = true;
    bool modified = true;
    bool operator==(const DetailsColumnVisibility&) const = default;
  };
  enum class DetailsColumn : std::uint8_t { TYPE, SIZE, MODIFIED };
  using DetailsColumnOrder = std::array<DetailsColumn, 3>;
  inline constexpr DetailsColumnOrder kDefaultDetailsColumnOrder{
      DetailsColumn::TYPE, DetailsColumn::SIZE, DetailsColumn::MODIFIED
  };
  inline bool validDetailsColumnOrder(DetailsColumnOrder order) {
    std::ranges::sort(order);
    return order == kDefaultDetailsColumnOrder;
  }
  inline std::string_view detailsColumnName(DetailsColumn column) {
    switch (column) {
    case DetailsColumn::TYPE:
      return "type";
    case DetailsColumn::SIZE:
      return "size";
    case DetailsColumn::MODIFIED:
      return "modified";
    }
    return {};
  }
  inline std::optional<DetailsColumn> detailsColumnFromName(std::string_view name) {
    for (const auto column : kDefaultDetailsColumnOrder)
      if (detailsColumnName(column) == name)
        return column;
    return std::nullopt;
  }
  inline bool detailsColumnVisible(DetailsColumnVisibility visibility, DetailsColumn column) {
    switch (column) {
    case DetailsColumn::TYPE:
      return visibility.type;
    case DetailsColumn::SIZE:
      return visibility.size;
    case DetailsColumn::MODIFIED:
      return visibility.modified;
    }
    return false;
  }
  inline std::optional<DetailsColumnOrder>
  moveDetailsColumn(DetailsColumnOrder order, DetailsColumn column, int direction, DetailsColumnVisibility visible) {
    if (!validDetailsColumnOrder(order)
        || (direction != -1 && direction != 1)
        || !detailsColumnVisible(visible, column))
      return std::nullopt;
    auto* const found = std::ranges::find(order, column);
    if (found == order.end())
      return std::nullopt;
    const auto source = found - order.begin();
    for (auto target = source + direction; target >= 0 && target < static_cast<std::ptrdiff_t>(order.size());
         target += direction) {
      if (detailsColumnVisible(visible, order[static_cast<std::size_t>(target)])) {
        std::swap(order[static_cast<std::size_t>(source)], order[static_cast<std::size_t>(target)]);
        return order;
      }
    }
    return std::nullopt;
  }
  enum class SortField : std::uint8_t { NAME, SIZE, MODIFIED, TYPE };
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
