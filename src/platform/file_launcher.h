#pragma once

#include "fs/location.h"

#include <functional>

namespace files {
  class FileLauncher {
  public:
    FileLauncher();
    ~FileLauncher();
    FileLauncher(const FileLauncher&) = delete;
    FileLauncher& operator=(const FileLauncher&) = delete;
    void open(const Location& location);
    void setChangedCallback(std::function<void(const std::string&)> callback);
    bool busy() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
