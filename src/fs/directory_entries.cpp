#include "fs/directory_entries.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

struct luciole::DirectoryEntries::Storage {
  struct Block {
    std::shared_ptr<const std::vector<DirectoryEntry>> entries;
    std::size_t end;
  };
  std::vector<Block> blocks;
  std::vector<const DirectoryEntry*> rows;
  std::size_t size = 0;
  bool projected = false;
};

luciole::DirectoryEntries::DirectoryEntries(std::vector<DirectoryEntry> entries) {
  if (entries.empty()) {
    return;
  }
  auto storage = std::make_shared<Storage>();
  storage->size = entries.size();
  storage->blocks.push_back({std::make_shared<const std::vector<DirectoryEntry>>(std::move(entries)), storage->size});
  m_storage = std::move(storage);
}

luciole::DirectoryEntries::DirectoryEntries(std::initializer_list<DirectoryEntry> entries)
    : DirectoryEntries(std::vector<DirectoryEntry>(entries)) {}

std::size_t luciole::DirectoryEntries::size() const { return m_storage ? m_storage->size : 0; }

const luciole::DirectoryEntry& luciole::DirectoryEntries::operator[](std::size_t index) const {
  if (m_storage->projected) {
    return *m_storage->rows[index];
  }
  const auto& blocks = m_storage->blocks;
  if (blocks.size() == 1) {
    return (*blocks.front().entries)[index];
  }
  const auto block = std::ranges::upper_bound(blocks, index, {}, &Storage::Block::end);
  return (*block->entries)[index - (block->end - block->entries->size())];
}

const luciole::DirectoryEntry& luciole::DirectoryEntries::at(std::size_t index) const {
  if (index >= size()) {
    throw std::out_of_range("directory entry index");
  }
  return (*this)[index];
}

luciole::DirectoryEntries luciole::DirectoryEntries::concatenate(std::vector<DirectoryEntries> sources) {
  std::erase_if(sources, [](const auto& source) { return source.empty(); });
  if (sources.empty()) {
    return {};
  }
  if (sources.size() == 1) {
    return sources.front();
  }
  if (std::ranges::any_of(sources, [](const auto& source) { return source.m_storage->projected; })) {
    std::vector<Index> rows;
    for (std::size_t source = 0; source < sources.size(); ++source) {
      for (std::size_t entry = 0; entry < sources[source].size(); ++entry) {
        rows.push_back({source, entry});
      }
    }
    return project(std::move(sources), rows);
  }
  auto storage = std::make_shared<Storage>();
  for (const auto& source : sources) {
    for (const auto& block : source.m_storage->blocks) {
      storage->size += block.entries->size();
      storage->blocks.push_back({block.entries, storage->size});
    }
  }
  return DirectoryEntries(std::move(storage));
}

luciole::DirectoryEntries
luciole::DirectoryEntries::project(std::vector<DirectoryEntries> sources, const std::vector<Index>& rows) {
  if (rows.empty()) {
    return {};
  }
  auto storage = std::make_shared<Storage>();
  storage->projected = true;
  storage->size = rows.size();
  std::unordered_set<const std::vector<DirectoryEntry>*> retained;
  for (const auto& source : sources) {
    if (!source.m_storage) {
      continue;
    }
    for (const auto& block : source.m_storage->blocks) {
      if (retained.insert(block.entries.get()).second) {
        storage->blocks.push_back(block);
      }
    }
  }
  storage->rows.reserve(rows.size());
  for (const auto& row : rows) {
    storage->rows.push_back(&sources.at(row.source).at(row.entry));
  }
  return DirectoryEntries(std::move(storage));
}
