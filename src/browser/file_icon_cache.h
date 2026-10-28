#pragma once

#include "fs/file_icon_source.h"
#include "render/core/image_file_loader.h"

#include <functional>
#include <memory>
#include <optional>

namespace files {
  struct FileIconImage {
    std::optional<LoadedImageFile> pixels;
    std::string path;
    bool symbolic = false;
    bool ready = false;
  };

  // Main-context cache. Workers resolve/decode; cells only present prepared pixels.
  class FileIconCache {
  public:
    FileIconCache();
    ~FileIconCache();
    FileIconCache(const FileIconCache&) = delete;
    FileIconCache& operator=(const FileIconCache&) = delete;

    std::shared_ptr<const FileIconImage> request(const FileIconSource& source, int pixels);
    void invalidate();
    void setChangedCallback(std::function<void()> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
