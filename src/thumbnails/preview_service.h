#pragma once

#include "thumbnails/preview_metadata.h"
#include "thumbnails/thumbnail_service.h"

namespace luciole {
  enum class PreviewStatus { Empty, Loading, Ready, Unavailable, Error };
  struct PreviewSnapshot {
    std::optional<DirectoryEntry> entry;
    PreviewStatus status = PreviewStatus::Empty;
    std::string text, message;
    std::shared_ptr<const FileIconImage> image;
    bool imageUpgradeFailed = false;
    MetadataFields metadata;
  };
  // One current selection and at most one text worker. Superseded work never publishes. Images come from the shared
  // ThumbnailService, whose owner forwards its change notifications to thumbnailsChanged().
  class PreviewService {
  public:
    explicit PreviewService(ThumbnailService& thumbnails);
    ~PreviewService();
    void show(std::optional<DirectoryEntry> entry, int pixels, std::shared_ptr<const FileIconImage> prepared = {});
    void clear();
    void thumbnailsChanged();
    void setChangedCallback(std::function<void()> callback);
    const PreviewSnapshot& snapshot() const;
    bool busy() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
