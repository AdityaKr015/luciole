#include "browser/directory_model.h"

#include "browser/entry_details.h"
#include "browser/selection_tools.h"
#include "fs/file_visibility.h"
#include "fs/name_matching.h"

#include <algorithm>
#include <gio/gio.h>
#include <limits>
#include <utility>

bool luciole::DirectoryModel::applySnapshot(
    DirectorySnapshot snapshot, std::unordered_map<EntryId, EntryId> parents, bool preserveRootOrder
) {
  if (snapshot.generation < m_snapshot.generation) {
    return false;
  }
  if (snapshot.location != m_snapshot.location) {
    m_selection = {};
    m_scrollAnchor.reset();
    m_selectionAvailable = false;
  }
  if (snapshot.status == DirectoryStatus::Idle || snapshot.status == DirectoryStatus::Cancelled) {
    m_selectionAvailable = false;
  } else if (snapshot.status == DirectoryStatus::Loading && snapshot.incrementalResults) {
    m_selectionAvailable = !snapshot.entries.empty();
  } else if (snapshot.status == DirectoryStatus::Ready || snapshot.status == DirectoryStatus::Error) {
    m_selectionAvailable = true;
  }
  m_preserveRootOrder = preserveRootOrder;
  m_treeParents = std::move(parents);
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
    if (entry.symlink || entry.type == luciole::FileType::Symlink) {
      typeLabel = "Link";
    } else if (entry.type == luciole::FileType::Directory) {
      typeLabel = "Folder";
    } else if (entry.type == luciole::FileType::Special) {
      typeLabel = "Special";
    } else if (entry.type == luciole::FileType::Shortcut) {
      typeLabel = "Shortcut";
    } else if (entry.type == luciole::FileType::Mountable) {
      typeLabel = "Mount";
    } else if (entry.type == luciole::FileType::Regular) {
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
    std::array<std::optional<std::string>, 3> detailKeys;
    for (std::size_t k = 0; k < detailKeys.size(); ++k) {
      const auto column = k == 0 ? DetailsColumn::Owner : k == 1 ? DetailsColumn::Group : DetailsColumn::Location;
      const bool known = k == 0 ? entry.details.owner.has_value()
          : k == 1              ? entry.details.group.has_value()
                                : entry.location.parent().has_value();
      if (known) {
        const auto value = foldedDisplayText(entryColumnText(entry, column));
        const GString collated(g_utf8_collate_key_for_filename(value.c_str(), -1));
        detailKeys[k] = collated.get();
      }
    }
    m_presentations.emplace(
        entry.id,
        Presentation{
            .sourceIndex = i,
            .sortKey = sortKey.get(),
            .filterKey = std::move(key),
            .typeLabel = std::move(typeLabel),
            .typeSortKey = std::move(typeSortKey),
            .detailSortKeys = std::move(detailKeys)
        }
    );
  }
  rebuildProjection();
  notify();
  return true;
}

void luciole::DirectoryModel::setChangedCallback(std::function<void()> callback) { m_changed = std::move(callback); }

void luciole::DirectoryModel::setSort(SortOptions options) {
  if (m_sort == options) {
    return;
  }
  m_sort = options;
  rebuildProjection();
  notify();
}

void luciole::DirectoryModel::setFilter(FilterOptions options) {
  if (m_filter == options) {
    return;
  }
  m_filterKey = foldedDisplayText(options.text);
  m_filter = std::move(options);
  rebuildProjection();
  notify();
}

const luciole::DirectoryEntry* luciole::DirectoryModel::entry(EntryId id) const {
  const auto it = m_presentations.find(id);
  return it == m_presentations.end() ? nullptr : &m_snapshot.entries[it->second.sourceIndex];
}

std::string_view luciole::DirectoryModel::typeLabel(EntryId id) const {
  const auto it = m_presentations.find(id);
  return it == m_presentations.end() ? std::string_view{} : it->second.typeLabel;
}

bool luciole::DirectoryModel::less(EntryId left, EntryId right) const {
  const auto& leftPresentation = m_presentations.at(left);
  const auto& rightPresentation = m_presentations.at(right);
  const auto& a = m_snapshot.entries[leftPresentation.sourceIndex];
  const auto& b = m_snapshot.entries[rightPresentation.sourceIndex];
  if (m_sort.directoriesFirst && (a.type == luciole::FileType::Directory) != (b.type == luciole::FileType::Directory)) {
    return a.type == luciole::FileType::Directory;
  }
  if (m_sort.field == SortField::Type) {
    const auto& first = leftPresentation.typeSortKey;
    const auto& second = rightPresentation.typeSortKey;
    if (first.has_value() != second.has_value()) {
      return first.has_value();
    }
    if (first && first != second) {
      return m_sort.descending ? first > second : first < second;
    }
  } else if (
      m_sort.field == SortField::Owner || m_sort.field == SortField::Group || m_sort.field == SortField::Location
  ) {
    const auto index = m_sort.field == SortField::Owner ? 0 : m_sort.field == SortField::Group ? 1 : 2;
    const auto& first = leftPresentation.detailSortKeys[index];
    const auto& second = rightPresentation.detailSortKeys[index];
    if (first.has_value() != second.has_value()) {
      return first.has_value();
    }
    if (first && first != second) {
      return m_sort.descending ? first > second : first < second;
    }
  } else if (m_sort.field != SortField::Name) {
    const auto first = entrySortNumber(a, m_sort.field);
    const auto second = entrySortNumber(b, m_sort.field);
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

void luciole::DirectoryModel::rebuildProjection() {
  m_rows.clear();
  m_rows.reserve(m_snapshot.entries.size());
  for (const auto& entry : m_snapshot.entries) {
    if ((!entry.hidden || m_filter.showHidden)
        && (m_filter.showBackups || !isEditorBackup(entry.name, entry.type))
        && m_presentations.at(entry.id).filterKey.contains(m_filterKey)) {
      m_rows.push_back(entry.id);
    }
  }
  std::ranges::sort(m_rows, [this](EntryId left, EntryId right) { return less(left, right); });
  if (!m_treeParents.empty() || m_preserveRootOrder) {
    std::unordered_map<EntryId, std::vector<EntryId>> children;
    std::vector<EntryId> roots;
    for (const auto id : m_rows) {
      if (const auto parent = treeParent(id)) {
        children[*parent].push_back(id);
      } else {
        roots.push_back(id);
      }
    }
    if (m_preserveRootOrder) {
      std::ranges::sort(roots, {}, [&](EntryId id) { return m_presentations.at(id).sourceIndex; });
    }
    m_rows.clear();
    std::unordered_set<EntryId> visited;
    const auto append = [&](this auto&& self, EntryId id, std::size_t depth) -> void {
      if (depth > 32 || !visited.insert(id).second) {
        return;
      }
      m_rows.push_back(id);
      if (const auto found = children.find(id); found != children.end()) {
        for (const auto child : found->second) {
          self(child, depth + 1);
        }
      }
    };
    for (const auto id : roots) {
      append(id, 0);
    }
  }
  m_positions.clear();
  m_positions.reserve(m_rows.size());
  for (std::size_t i = 0; i < m_rows.size(); ++i) {
    m_positions.emplace(m_rows[i], i);
  }
  reconcileSelection();
}

std::optional<luciole::EntryId> luciole::DirectoryModel::treeParent(EntryId id) const {
  const auto found = m_treeParents.find(id);
  return found == m_treeParents.end() ? std::nullopt : std::optional(found->second);
}
std::size_t luciole::DirectoryModel::treeDepth(EntryId id) const {
  std::size_t depth = 0;
  while (const auto parent = treeParent(id)) {
    id = *parent;
    if (++depth >= 32) {
      break;
    }
  }
  return depth;
}

bool luciole::DirectoryModel::canSelect() const { return m_selectionAvailable; }

void luciole::DirectoryModel::reconcileSelection() {
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

std::vector<luciole::EntryId> luciole::DirectoryModel::selectedIds() const {
  std::vector<EntryId> ids;
  ids.reserve(m_selection.selected.size());
  for (const auto id : m_rows) {
    if (m_selection.selected.contains(id)) {
      ids.push_back(id);
    }
  }
  return ids;
}

luciole::SelectionSummary luciole::DirectoryModel::selectionSummary() const {
  SelectionSummary summary;
  for (const auto id : m_selection.selected) {
    const auto* item = entry(id);
    if (item == nullptr) {
      continue;
    }
    if (!item->symlink && item->type == luciole::FileType::Directory) {
      ++summary.folders;
    } else if (!item->symlink && item->type == luciole::FileType::Regular) {
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

bool luciole::DirectoryModel::select(EntryId id, SelectionMode mode) {
  if (!canSelect() || !m_positions.contains(id)) {
    return false;
  }
  if (mode == SelectionMode::Replace) {
    m_selection.selected = {id};
    m_selection.anchor = id;
  } else if (mode == SelectionMode::Toggle) {
    if (m_selection.selected.contains(id)) {
      m_selection.selected.erase(id);
    } else {
      m_selection.selected.insert(id);
    }
    m_selection.anchor = id;
  } else {
    const auto anchor = m_selection.anchor.value_or(m_selection.focused.value_or(id));
    m_selection.anchor = anchor;
    if (mode == SelectionMode::Range) {
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

void luciole::DirectoryModel::selectAll() {
  if (!canSelect()) {
    return;
  }
  m_selection.selected.clear();
  m_selection.selected.insert(m_rows.begin(), m_rows.end());
  notify();
}

bool luciole::DirectoryModel::invertSelection() {
  if (!canSelect()) {
    return false;
  }
  std::unordered_set<EntryId> selected;
  for (const auto id : m_rows) {
    if (!m_selection.selected.contains(id)) {
      selected.insert(id);
    }
  }
  m_selection.selected = std::move(selected);
  notify();
  return true;
}
std::optional<std::string> luciole::DirectoryModel::selectPattern(const std::string& pattern) {
  if (const auto error = selectionPatternProblem(pattern)) {
    return error;
  }
  if (!canSelect()) {
    return "The listing is not available for selection.";
  }
  const auto folded = foldedDisplayText(pattern);
  const std::unique_ptr<GPatternSpec, decltype(&g_pattern_spec_free)> spec(
      g_pattern_spec_new(folded.c_str()), g_pattern_spec_free
  );
  std::unordered_set<EntryId> selected;
  for (const auto id : m_rows) {
    if (g_pattern_spec_match_string(spec.get(), m_presentations.at(id).filterKey.c_str())) {
      selected.insert(id);
    }
  }
  m_selection.selected = std::move(selected);
  notify();
  return std::nullopt;
}
std::optional<luciole::EntryId> luciole::DirectoryModel::prefixMatch(const std::string& prefix, bool cycle) const {
  if (!canSelect() || prefix.empty() || m_rows.empty()) {
    return std::nullopt;
  }
  const auto folded = foldedDisplayText(prefix);
  std::size_t start = 0;
  if (m_selection.focused) {
    const auto found = m_positions.find(*m_selection.focused);
    if (found != m_positions.end()) {
      start = (found->second + (cycle ? 1 : 0)) % m_rows.size();
    }
  }
  for (std::size_t offset = 0; offset < m_rows.size(); ++offset) {
    const auto id = m_rows[(start + offset) % m_rows.size()];
    if (m_presentations.at(id).filterKey.starts_with(folded)) {
      return id;
    }
  }
  return std::nullopt;
}

void luciole::DirectoryModel::clearSelection() {
  m_selection.selected.clear();
  m_selection.anchor.reset();
  notify();
}

bool luciole::DirectoryModel::replaceSelection(std::span<const EntryId> ids) {
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

bool luciole::DirectoryModel::setFocus(std::optional<EntryId> id) {
  if (id && (!canSelect() || !m_positions.contains(*id))) {
    return false;
  }
  m_selection.focused = id;
  notify();
  return true;
}

bool luciole::DirectoryModel::setScrollAnchor(std::optional<EntryId> id) {
  if (id && (!canSelect() || !m_positions.contains(*id))) {
    return false;
  }
  m_scrollAnchor = id;
  notify();
  return true;
}

void luciole::DirectoryModel::notify() const {
  const auto callback = m_changed;
  if (callback) {
    callback();
  }
}

void luciole::DirectoryModel::restoreInteraction(SelectionState selection, std::optional<EntryId> scroll) {
  if (!m_selectionAvailable) {
    return;
  }
  const auto visible = [this](EntryId id) { return m_positions.contains(id); };
  std::erase_if(selection.selected, [&](EntryId id) { return !visible(id); });
  if (selection.focused && !visible(*selection.focused)) {
    selection.focused.reset();
  }
  if (selection.anchor && !visible(*selection.anchor)) {
    selection.anchor.reset();
  }
  if (scroll && !visible(*scroll)) {
    scroll.reset();
  }
  m_selection = std::move(selection);
  m_scrollAnchor = scroll;
  notify();
}
