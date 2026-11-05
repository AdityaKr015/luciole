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
  enum class ViewMode : std::uint8_t { LIST, GRID, MILLER };
  enum class ActivationMode : std::uint8_t { SingleClick, DoubleClick };
  enum class TypingMode : std::uint8_t { SEARCH, TYPEAHEAD };
  // Complete column allocations, including the gap after their contents. Zero
  // is the automatic Name width; metadata always has an explicit allocation.
  using DetailsColumnWidths = std::array<int, 4>;
  inline constexpr DetailsColumnWidths kDefaultDetailsColumnWidths{0, 152, 118, 156};
  inline constexpr std::array<std::string_view, 4> kDetailsColumnWidthKeys{
      "name_column_width", "type_column_width", "size_column_width", "modified_column_width"
  };
  inline int minimumDetailsColumnWidth(std::size_t index) { return index == 0 ? 160 : 64; }
  inline int maximumDetailsColumnWidth(std::size_t index) { return index == 0 ? 16384 : 2048; }
  inline bool validDetailsColumnWidth(std::size_t index, int width) {
    return index < 4
        && ((index == 0 && width == 0)
            || (width >= minimumDetailsColumnWidth(index) && width <= maximumDetailsColumnWidth(index)));
  }
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
    for (const auto column : kDefaultDetailsColumnOrder) {
      if (detailsColumnName(column) == name) {
        return column;
      }
    }
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
        || !detailsColumnVisible(visible, column)) {
      return std::nullopt;
    }
    auto* const found = std::ranges::find(order, column);
    if (found == order.end()) {
      return std::nullopt;
    }
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
  inline std::optional<DetailsColumnOrder>
  insertDetailsColumnBefore(DetailsColumnOrder order, DetailsColumn column, std::optional<DetailsColumn> before) {
    if (!validDetailsColumnOrder(order)
        || !std::ranges::contains(order, column)
        || (before && !std::ranges::contains(order, *before))) {
      return std::nullopt;
    }
    if (before == column) {
      return order;
    }
    DetailsColumnOrder result{};
    std::size_t index = 0;
    for (const auto candidate : order) {
      if (candidate == column) {
        continue;
      }
      if (before == candidate) {
        result[index++] = column;
      }
      result[index++] = candidate;
    }
    if (!before) {
      result[index] = column;
    }
    return result;
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
