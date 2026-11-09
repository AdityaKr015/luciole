#pragma once

#include "fs/file_icon_source.h"
#include "fs/location.h"

#include <functional>
#include <memory>
#include <vector>

using GAppInfo = struct _GAppInfo;

namespace files {
  struct ApplicationChoice {
    std::string id, name;
    FileIconSource icon;
    std::shared_ptr<GAppInfo> app;
  };
  class FileLauncher {
  public:
    FileLauncher();
    ~FileLauncher();
    FileLauncher(const FileLauncher&) = delete;
    FileLauncher& operator=(const FileLauncher&) = delete;
    void open(const Location& location);
    void applications(std::string mime, std::function<void(std::vector<ApplicationChoice>)> callback);
    void cancelApplications();
    void openWith(const Location& location, const ApplicationChoice& application);
    void setChangedCallback(std::function<void(const std::string&)> callback);
    bool busy() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
