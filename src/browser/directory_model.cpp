#include "browser/directory_model.h"

#include "browser/selection_tools.h"
#include "fs/name_matching.h"

#include <algorithm>
#include <limits>
#include <utility>

bool files::DirectoryModel::applySnapshot(DirectorySnapshot snapshot) {
  if (snapshot.generation < m_snapshot.generation) {
    return false;
  }
  if (snapshot.location != m_snapshot.location) {
    m_selection = {};
    m_scrollAnchor.reset();
    m_selectionAvailable = false;
  }
  if (snapshot.status == DirectoryStatus::IDLE || snapshot.status == DirectoryStatus::CANCELLED) {
    m_selectionAvailable = false;
  } else if (snapshot.status == DirectoryStatus::LOADING && snapshot.incrementalResults) {
    m_selectionAvailable = !snapshot.entries.empty();
  } else if (snapshot.status == DirectoryStatus::READY || snapshot.status == DirectoryStatus::ERROR) {
    m_selectionAvailable = true;
  }
  m_snapshot = std::move(snapshot);
  m_presentations.clear();
  m_presentations.reserve(m_snapshot.entries.size());
  std::unordered_map<std::string, std::string> typeLabels;
  for (std::size_t i = 0; i < m_snapshot.entries.size(); ++i) {
    const auto& entry = m_snapshot.entries[i];
    auto key = foldedDisplayText(entry.displayName);
    const GString sortKey(g_utf8_collate_key_for_filename(key.c_str(), -1));
    std::string typeLabel;
    bool knownType = true;
    if (entry.symlink || entry.type == G_FILE_TYPE_SYMBOLIC_LINK) {
      typeLabel = "Link";
    } else if (entry.type == G_FILE_TYPE_DIRECTORY) {
      typeLabel = "Folder";
    } else if (entry.type == G_FILE_TYPE_SPECIAL) {
      typeLabel = "Special";
    } else if (entry.type == G_FILE_TYPE_SHORTCUT) {
      typeLabel = "Shortcut";
    } else if (entry.type == G_FILE_TYPE_MOUNTABLE) {
      typeLabel = "Mount";
    } else if (entry.type == G_FILE_TYPE_REGULAR) {
      knownType = entry.contentType && !g_content_type_is_unknown(entry.contentType->c_str());
      typeLabel = "File";
      if (knownType) {
        auto [label, inserted] = typeLabels.try_emplace(*entry.contentType);
        if (inserted) {
          const GString description(g_content_type_get_description(entry.contentType->c_str()));
          label->second = description && *description ? description.get() : *entry.contentType;
        }
        typeLabel = label->second;
      }
    } else {
      typeLabel = "Unknown";
      knownType = false;
    }
    std::optional<std::string> typeSortKey;
    if (knownType) {
      const GString collated(g_utf8_collate_key_for_filename(foldedDisplayText(typeLabel).c_str(), -1));
      typeSortKey = collated.get();
    }
    m_presentations.emplace(
        entry.id,
        Presentation{
            .sourceIndex = i,
            .sortKey = sortKey.get(),
            .filterKey = std::move(key),
            .typeLabel = std::move(typeLabel),
            .typeSortKey = std::move(typeSortKey)
        }
    );
  }
  rebuildProjection();
  notify();
  return true;
}

void files::DirectoryModel::setChangedCallback(std::function<void()> callback) { m_changed = std::move(callback); }

void files::DirectoryModel::setSort(SortOptions options) {
  if (m_sort == options) {
    return;
  }
  m_sort = options;
  rebuildProjection();
  notify();
}

void files::DirectoryModel::setFilter(FilterOptions options) {
  if (m_filter == options) {
    return;
  }
  m_filterKey = foldedDisplayText(options.text);
  m_filter = std::move(options);
  rebuildProjection();
  notify();
}

const files::DirectoryEntry* files::DirectoryModel::entry(EntryId id) const {
  const auto it = m_presentations.find(id);
  return it == m_presentations.end() ? nullptr : &m_snapshot.entries[it->second.sourceIndex];
}

std::string_view files::DirectoryModel::typeLabel(EntryId id) const {
  const auto it = m_presentations.find(id);
  return it == m_presentations.end() ? std::string_view{} : it->second.typeLabel;
}

bool files::DirectoryModel::less(EntryId left, EntryId right) const {
  const auto& leftPresentation = m_presentations.at(left);
  const auto& rightPresentation = m_presentations.at(right);
  const auto& a = m_snapshot.entries[leftPresentation.sourceIndex];
  const auto& b = m_snapshot.entries[rightPresentation.sourceIndex];
  if (m_sort.directoriesFirst && (a.type == G_FILE_TYPE_DIRECTORY) != (b.type == G_FILE_TYPE_DIRECTORY)) {
    return a.type == G_FILE_TYPE_DIRECTORY;
  }
  if (m_sort.field == SortField::TYPE) {
    const auto& first = leftPresentation.typeSortKey;
    const auto& second = rightPresentation.typeSortKey;
    if (first.has_value() != second.has_value())
      return first.has_value();
    if (first && first != second)
      return m_sort.descending ? first > second : first < second;
  } else if (m_sort.field != SortField::NAME) {
    const auto& first = m_sort.field == SortField::SIZE ? a.size : a.modifiedSeconds;
    const auto& second = m_sort.field == SortField::SIZE ? b.size : b.modifiedSeconds;
    // Unknown metadata stays last in both directions; zero is a real value.
    if (first.has_value() != second.has_value()) {
      return first.has_value();
    }
    if (first && first != second) {
      return m_sort.descending ? first > second : first < second;
    }
  }
  int comparison = leftPresentation.sortKey.compare(rightPresentation.sortKey);
  if (comparison == 0) {
    comparison = a.location.uri().compare(b.location.uri());
  }
  if (comparison == 0) {
    return left < right;
  }
  return m_sort.descending ? comparison > 0 : comparison < 0;
}

void files::DirectoryModel::rebuildProjection() {
  m_rows.clear();
  m_rows.reserve(m_snapshot.entries.size());
  for (const auto& entry : m_snapshot.entries) {
    if ((!entry.hidden || m_filter.showHidden) && m_presentations.at(entry.id).filterKey.contains(m_filterKey)) {
      m_rows.push_back(entry.id);
    }
  }
  std::ranges::sort(m_rows, [this](EntryId left, EntryId right) { return less(left, right); });
  m_positions.clear();
  m_positions.reserve(m_rows.size());
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    m_positions.emplace(m_rows[i], i);
  }
  reconcileSelection();
}

bool files::DirectoryModel::canSelect() const { return m_selectionAvailable; }

void files::DirectoryModel::reconcileSelection() {
  if (!canSelect()) {
    m_selection = {};
    m_scrollAnchor.reset();
    return;
  }
  std::erase_if(m_selection.selected, [this](EntryId id) { return !m_positions.contains(id); });
  for (auto* id : {&m_selection.focused, &m_selection.anchor, &m_scrollAnchor}) {
    if (*id && !m_positions.contains(**id)) {
      id->reset();
    }
  }
}

std::vector<files::EntryId> files::DirectoryModel::selectedIds() const {
  std::vector<EntryId> ids;
  ids.reserve(m_selection.selected.size());
  for (const auto id : m_rows) {
    if (m_selection.selected.contains(id)) {
      ids.push_back(id);
    }
  }
  return ids;
}

files::SelectionSummary files::DirectoryModel::selectionSummary() const {
  SelectionSummary summary;
  for (const auto id : m_selection.selected) {
    const auto* item = entry(id);
    if (item == nullptr)
      continue;
    if (!item->symlink && item->type == G_FILE_TYPE_DIRECTORY) {
      ++summary.folders;
    } else if (!item->symlink && item->type == G_FILE_TYPE_REGULAR) {
      ++summary.files;
      if (!item->size) {
        ++summary.unknownSizes;
      } else if (*item->size > std::numeric_limits<std::uint64_t>::max() - summary.bytes) {
        summary.bytes = std::numeric_limits<std::uint64_t>::max();
        summary.overflow = true;
      } else {
        summary.bytes += *item->size;
      }
    } else {
      ++summary.others;
    }
  }
  return summary;
}

bool files::DirectoryModel::select(EntryId id, SelectionMode mode) {
  if (!canSelect() || !m_positions.contains(id)) {
    return false;
  }
  if (mode == SelectionMode::REPLACE) {
    m_selection.selected = {id};
    m_selection.anchor = id;
  } else if (mode == SelectionMode::TOGGLE) {
    if (m_selection.selected.contains(id)) {
      m_selection.selected.erase(id);
    } else {
      m_selection.selected.insert(id);
    }
    m_selection.anchor = id;
  } else {
    const auto anchor = m_selection.anchor.value_or(m_selection.focused.value_or(id));
    m_selection.anchor = anchor;
    if (mode == SelectionMode::RANGE) {
      m_selection.selected.clear();
    }
    const auto [first, last] = std::minmax(m_positions.at(anchor), m_positions.at(id));
    for (std::size_t i = first; i <= last; ++i) {
      m_selection.selected.insert(m_rows[i]);
    }
  }
  m_selection.focused = id;
  notify();
  return true;
}

void files::DirectoryModel::selectAll() {
  if (!canSelect()) {
    return;
  }
  m_selection.selected.clear();
  m_selection.selected.insert(m_rows.begin(), m_rows.end());
  notify();
}

bool files::DirectoryModel::invertSelection() {
  if (!canSelect())
    return false;
  std::unordered_set<EntryId> selected;
  for (const auto id : m_rows)
    if (!m_selection.selected.contains(id))
      selected.insert(id);
  m_selection.selected = std::move(selected);
  notify();
  return true;
}
std::optional<std::string> files::DirectoryModel::selectPattern(const std::string& pattern) {
  if (const auto error = selectionPatternProblem(pattern))
    return error;
  if (!canSelect())
    return "The listing is not available for selection.";
  const auto folded = foldedDisplayText(pattern);
  const std::unique_ptr<GPatternSpec, decltype(&g_pattern_spec_free)> spec(
      g_pattern_spec_new(folded.c_str()), g_pattern_spec_free
  );
  std::unordered_set<EntryId> selected;
  for (const auto id : m_rows)
    if (g_pattern_spec_match_string(spec.get(), m_presentations.at(id).filterKey.c_str()))
      selected.insert(id);
  m_selection.selected = std::move(selected);
  notify();
  return std::nullopt;
}
std::optional<files::EntryId> files::DirectoryModel::prefixMatch(const std::string& prefix, bool cycle) const {
  if (!canSelect() || prefix.empty() || m_rows.empty())
    return std::nullopt;
  const auto folded = foldedDisplayText(prefix);
  std::size_t start = 0;
  if (m_selection.focused) {
    const auto found = m_positions.find(*m_selection.focused);
    if (found != m_positions.end())
      start = (found->second + (cycle ? 1 : 0)) % m_rows.size();
  }
  for (std::size_t offset = 0; offset < m_rows.size(); ++offset) {
    const auto id = m_rows[(start + offset) % m_rows.size()];
    if (m_presentations.at(id).filterKey.starts_with(folded))
      return id;
  }
  return std::nullopt;
}

void files::DirectoryModel::clearSelection() {
  m_selection.selected.clear();
  m_selection.anchor.reset();
  notify();
}

bool files::DirectoryModel::replaceSelection(std::span<const EntryId> ids) {
  if (!canSelect() || std::ranges::any_of(ids, [this](EntryId id) { return !m_positions.contains(id); })) {
    return false;
  }
  std::unordered_set<EntryId> selected(ids.begin(), ids.end());
  if (selected == m_selection.selected) {
    return true;
  }
  m_selection.selected = std::move(selected);
  m_selection.anchor = ids.empty() ? std::nullopt : std::optional(ids.front());
  if (!ids.empty() && (!m_selection.focused || !m_selection.selected.contains(*m_selection.focused))) {
    m_selection.focused = ids.back();
  }
  notify();
  return true;
}

bool files::DirectoryModel::setFocus(std::optional<EntryId> id) {
  if (id && (!canSelect() || !m_positions.contains(*id))) {
    return false;
  }
  m_selection.focused = id;
  notify();
  return true;
}

bool files::DirectoryModel::setScrollAnchor(std::optional<EntryId> id) {
  if (id && (!canSelect() || !m_positions.contains(*id))) {
    return false;
  }
  m_scrollAnchor = id;
  notify();
  return true;
}

void files::DirectoryModel::notify() const {
  const auto callback = m_changed;
  if (callback) {
    callback();
  }
}

void files::DirectoryModel::restoreInteraction(SelectionState selection, std::optional<EntryId> scroll) {
  if (!m_selectionAvailable)
    return;
  const auto visible = [this](EntryId id) { return m_positions.contains(id); };
  std::erase_if(selection.selected, [&](EntryId id) { return !visible(id); });
  if (selection.focused && !visible(*selection.focused))
    selection.focused.reset();
  if (selection.anchor && !visible(*selection.anchor))
    selection.anchor.reset();
  if (scroll && !visible(*scroll))
    scroll.reset();
  m_selection = std::move(selection);
  m_scrollAnchor = scroll;
  notify();
}
