#pragma once

#include "browser/thumbnail_service.h"

namespace files {
  enum class PreviewStatus { EMPTY, LOADING, READY, UNAVAILABLE, ERROR };
  struct PreviewSnapshot {
    std::optional<DirectoryEntry> entry;
    PreviewStatus status = PreviewStatus::EMPTY;
    std::string text, message;
    std::shared_ptr<const FileIconImage> image;
    bool imageUpgradeFailed = false;
  };
  // One current selection and at most one text worker. Superseded work never publishes.
  class PreviewService {
  public:
    PreviewService();
    ~PreviewService();
    void show(
        std::optional<DirectoryEntry> entry, int pixels, ThumbnailOptions options,
        std::shared_ptr<const FileIconImage> prepared = {}
    );
    void clear();
    void setChangedCallback(std::function<void()> callback);
    const PreviewSnapshot& snapshot() const;
    bool busy() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
