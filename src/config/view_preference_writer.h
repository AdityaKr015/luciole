#pragma once

#include "config/settings.h"

#include <functional>
#include <memory>

namespace files {
  // Serial worker writes, coalescing rapid changes to the latest requested view.
  class ViewPreferenceWriter {
  public:
    explicit ViewPreferenceWriter(ConfigPaths paths);
    ~ViewPreferenceWriter();
    ViewPreferenceWriter(const ViewPreferenceWriter&) = delete;
    ViewPreferenceWriter& operator=(const ViewPreferenceWriter&) = delete;
    void save(ViewMode mode);
    void saveGridIconSize(int size);
    [[nodiscard]] bool busy() const;
    void setChangedCallback(std::function<void(const std::string&)> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
