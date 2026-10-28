#include "browser/directory_model.h"

#include <algorithm>
#include <utility>

namespace {
  std::string folded(const std::string& text) {
    const files::GString valid(g_utf8_make_valid(text.c_str(), static_cast<gssize>(text.size())));
    const files::GString lower(g_utf8_casefold(valid.get(), -1));
    const files::GString normalized(g_utf8_normalize(lower.get(), -1, G_NORMALIZE_DEFAULT_COMPOSE));
    return normalized.get();
  }
} // namespace

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
  } else if (snapshot.status == DirectoryStatus::READY || snapshot.status == DirectoryStatus::ERROR) {
    m_selectionAvailable = true;
  }
  m_snapshot = std::move(snapshot);
  m_presentations.clear();
  m_presentations.reserve(m_snapshot.entries.size());
  for (std::size_t i = 0; i < m_snapshot.entries.size(); ++i) {
    const auto& entry = m_snapshot.entries[i];
    auto key = folded(entry.displayName);
    const GString sortKey(g_utf8_collate_key_for_filename(key.c_str(), -1));
    m_presentations.emplace(
        entry.id, Presentation{.sourceIndex = i, .sortKey = sortKey.get(), .filterKey = std::move(key)}
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
  m_filterKey = folded(options.text);
  m_filter = std::move(options);
  rebuildProjection();
  notify();
}

const files::DirectoryEntry* files::DirectoryModel::entry(EntryId id) const {
  const auto it = m_presentations.find(id);
  return it == m_presentations.end() ? nullptr : &m_snapshot.entries[it->second.sourceIndex];
}

bool files::DirectoryModel::less(EntryId left, EntryId right) const {
  const auto& leftPresentation = m_presentations.at(left);
  const auto& rightPresentation = m_presentations.at(right);
  const auto& a = m_snapshot.entries[leftPresentation.sourceIndex];
  const auto& b = m_snapshot.entries[rightPresentation.sourceIndex];
  if (m_sort.directoriesFirst && (a.type == G_FILE_TYPE_DIRECTORY) != (b.type == G_FILE_TYPE_DIRECTORY)) {
    return a.type == G_FILE_TYPE_DIRECTORY;
  }
  if (m_sort.field != SortField::NAME) {
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

void files::DirectoryModel::clearSelection() {
  m_selection.selected.clear();
  m_selection.anchor.reset();
  notify();
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
