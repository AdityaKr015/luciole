#pragma once

#include "config/atomic_file.h"
#include "config/diagnostic.h"
#include "fs/location.h"

#include <vector>

namespace files {
  struct Bookmark {
    std::string label;
    Location location;
    bool operator==(const Bookmark&) const = default;
  };

  enum class ActivePane : std::uint8_t { PRIMARY, SECONDARY };
  struct SavedTab {
    Location primary;
    std::optional<Location> secondary;
    ActivePane activePane = ActivePane::PRIMARY;
    bool operator==(const SavedTab&) const = default;
  };

  struct SavedWindow {
    std::vector<SavedTab> tabs;
    std::size_t activeTab = 0;
    bool operator==(const SavedWindow&) const = default;
  };

  struct SessionState {
    std::vector<Bookmark> bookmarks;
    std::vector<SavedWindow> windows;
    bool placesCustomized = false;
    bool operator==(const SessionState&) const = default;
  };

  // Typed adaptation of Noctalia's StateStore, with no generic owner/key bag.
  // Single owning I/O thread; load before saving and marshal snapshots to UI.
  class StateStore {
  public:
    static constexpr std::size_t bookmarkLimit = 256;
    explicit StateStore(std::filesystem::path path);
    [[nodiscard]] static std::filesystem::path pathFromEnvironment();
    bool load(); // Failure preserves the last valid snapshot and the file.
    AtomicWriteState saveBookmarks(std::vector<Bookmark> bookmarks);
    AtomicWriteState saveWindows(std::vector<SavedWindow> windows);
    [[nodiscard]] const SessionState& state() const { return m_state; }
    [[nodiscard]] const ConfigIssues& issues() const { return m_issues; }

  private:
    AtomicWriteState save(SessionState proposed, bool bookmarks);
    [[nodiscard]] bool validPath() const;

    std::filesystem::path m_path;
    SessionState m_state;
    ConfigIssues m_issues;
    bool m_loaded = false;
  };
} // namespace files
