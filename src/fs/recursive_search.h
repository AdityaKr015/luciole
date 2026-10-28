#pragma once

#include "fs/directory_session.h"

namespace files {
  // Default-context GIO traversal, one directory and one 64-entry batch in flight.
  class RecursiveSearch {
  public:
    RecursiveSearch();
    ~RecursiveSearch();
    RecursiveSearch(const RecursiveSearch&) = delete;
    RecursiveSearch& operator=(const RecursiveSearch&) = delete;

    void start(const Location& root, const std::string& query, bool showHidden);
    void cancel();
    void setChangedCallback(std::function<void()> callback);
    [[nodiscard]] const DirectorySnapshot& snapshot() const;
    [[nodiscard]] std::size_t skippedFolders() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
