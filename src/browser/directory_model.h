#pragma once

#include "fs/directory_session.h"
#include "model/directory_options.h"

#include <functional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace luciole {
  using EntryId = std::uint64_t;

  enum class SelectionMode : std::uint8_t { Replace, Toggle, Range, AddRange };
  struct SelectionState {
    std::unordered_set<EntryId> selected;
    std::optional<EntryId> focused;
    std::optional<EntryId> anchor;
  };

  struct SelectionSummary {
    std::size_t files = 0;
    std::size_t folders = 0;
    std::size_t others = 0;
    std::uint64_t bytes = 0;
    std::size_t unknownSizes = 0;
    bool overflow = false;
    bool operator==(const SelectionSummary&) const = default;
  };

  // One model per DirectorySession, updated on the main thread by its observer.
  // The model owns its snapshot; borrowed rows/entries expire on mutation.
  class DirectoryModel {
  public:
    DirectoryModel() = default;
    DirectoryModel(const DirectoryModel&) = delete;
    DirectoryModel& operator=(const DirectoryModel&) = delete;
    DirectoryModel(DirectoryModel&&) = delete;
    DirectoryModel& operator=(DirectoryModel&&) = delete;

    // Returns false for an obsolete generation. Observers must not throw.
    bool applySnapshot(
        DirectorySnapshot snapshot, std::unordered_map<EntryId, EntryId> parents = {}, bool preserveRootOrder = false
    );
    std::optional<EntryId> treeParent(EntryId id) const;
    std::size_t treeDepth(EntryId id) const;
    void setChangedCallback(std::function<void()> callback);
    void setSort(SortOptions options);
    void setFilter(FilterOptions options);

    const DirectorySnapshot& snapshot() const { return m_snapshot; }
    const std::vector<EntryId>& rows() const { return m_rows; }
    const DirectoryEntry* entry(EntryId id) const;
    std::string_view typeLabel(EntryId id) const;
    const SortOptions& sort() const { return m_sort; }
    const FilterOptions& filter() const { return m_filter; }
    const SelectionState& selection() const { return m_selection; }
    std::optional<EntryId> scrollAnchor() const { return m_scrollAnchor; }
    std::vector<EntryId> selectedIds() const; // Visible order, for action dispatch.
    SelectionSummary selectionSummary() const;

    // Reject invisible or unknown IDs without changing focus or selection.
    bool select(EntryId id, SelectionMode mode = SelectionMode::Replace);
    bool selectionAvailable() const { return m_selectionAvailable; }
    void selectAll();
    bool invertSelection();
    // Case-folded, normalized visible-name glob. Bulk selection keeps focus and range anchor.
    std::optional<std::string> selectPattern(const std::string& pattern);
    std::optional<EntryId> prefixMatch(const std::string& prefix, bool cycle = false) const;
    void clearSelection();
    bool replaceSelection(std::span<const EntryId> ids);
    void restoreInteraction(SelectionState selection, std::optional<EntryId> scroll);
    bool setFocus(std::optional<EntryId> id);
    bool setScrollAnchor(std::optional<EntryId> id);

  private:
    struct Presentation {
      std::size_t sourceIndex = 0;
      std::string sortKey;
      std::string filterKey;
      std::string typeLabel;
      std::optional<std::string> typeSortKey;
      std::array<std::optional<std::string>, 3> detailSortKeys{};
    };

    void rebuildProjection();
    void reconcileSelection();
    void notify() const;
    bool canSelect() const;
    bool less(EntryId left, EntryId right) const;

    DirectorySnapshot m_snapshot;
    SortOptions m_sort;
    FilterOptions m_filter;
    std::string m_filterKey;
    std::unordered_map<EntryId, Presentation> m_presentations;
    std::vector<EntryId> m_rows;
    std::unordered_map<EntryId, EntryId> m_treeParents;
    std::unordered_map<EntryId, std::size_t> m_positions;
    SelectionState m_selection;
    std::optional<EntryId> m_scrollAnchor;
    bool m_selectionAvailable = false;
    bool m_preserveRootOrder = false;
    std::function<void()> m_changed;
  };
} // namespace luciole
