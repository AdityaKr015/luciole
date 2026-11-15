#pragma once

#include "fs/file_icon_source.h"
#include "fs/file_metadata.h"
#include "fs/location.h"
#include "fs/trash_info.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace luciole {
  struct DirectoryEntry {
    std::uint64_t id = 0;
    Location location;
    std::string name; // Original filename bytes, including non-UTF-8 names on Unix.
    std::string displayName;
    FileType type = FileType::Unknown;
    bool hidden = false;
    bool symlink = false;
    // Local listings leave access read/write/execute unknown; entryAccess() asks the filesystem when they matter.
    bool accessDeferred = false;
    std::optional<std::uint64_t> size;
    std::optional<std::uint64_t> modifiedSeconds;
    std::optional<std::string> etag;
    FileAccess access{};
    std::string parentDisplayName; // Search result context; never an operation address.
    SharedFileIcon icon = nullptr; // Shared between entries with equal artwork; null means none.
    // Set only for Trash items; most listings have none.
    std::shared_ptr<const TrashInfo> trash = nullptr;
    std::optional<FileIdentity> identity = std::nullopt;
    // Provider's regular MIME type, with its fast type as a fallback.
    InternedText contentType = nullptr;
    EntryMetadata details{};
    std::optional<ChangeRevision> contentRevision = std::nullopt;

    // The Trash facts, empty outside the Trash.
    const TrashInfo& trashInfo() const {
      static const TrashInfo none;
      return trash ? *trash : none;
    }
  };

  // Published entries are immutable. Copies retain payloads; producers publish a new collection.
  class DirectoryEntries {
  public:
    DirectoryEntries() = default;
    DirectoryEntries(std::vector<DirectoryEntry> entries);
    DirectoryEntries(std::initializer_list<DirectoryEntry> entries);

    struct Index {
      std::size_t source;
      std::size_t entry;
    };
    // Retains source payloads, without retaining intermediate projection/index tables.
    static DirectoryEntries concatenate(std::vector<DirectoryEntries> sources);
    static DirectoryEntries project(std::vector<DirectoryEntries> sources, const std::vector<Index>& rows);

    std::size_t size() const;
    bool empty() const { return size() == 0; }
    const DirectoryEntry& operator[](std::size_t index) const;
    const DirectoryEntry& at(std::size_t index) const;
    const DirectoryEntry& front() const { return (*this)[0]; }
    const DirectoryEntry& back() const { return (*this)[size() - 1]; }

    class Iterator {
    public:
      using value_type = DirectoryEntry;
      using difference_type = std::ptrdiff_t;
      using reference = const DirectoryEntry&;
      using pointer = const DirectoryEntry*;
      using iterator_category = std::forward_iterator_tag;
      using iterator_concept = std::forward_iterator_tag;
      Iterator() = default;
      reference operator*() const { return (*m_owner)[m_index]; }
      pointer operator->() const { return &**this; }
      Iterator& operator++() {
        ++m_index;
        return *this;
      }
      Iterator operator++(int) {
        const auto previous = *this;
        ++*this;
        return previous;
      }
      bool operator==(const Iterator&) const = default;

    private:
      friend DirectoryEntries;
      Iterator(const DirectoryEntries* owner, std::size_t index) : m_owner(owner), m_index(index) {}
      const DirectoryEntries* m_owner = nullptr;
      std::size_t m_index = 0;
    };
    Iterator begin() const { return Iterator(this, 0); }
    Iterator end() const { return Iterator(this, size()); }

  private:
    struct Storage;
    explicit DirectoryEntries(std::shared_ptr<const Storage> storage) : m_storage(std::move(storage)) {}
    std::shared_ptr<const Storage> m_storage;
  };
} // namespace luciole
