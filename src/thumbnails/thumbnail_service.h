#pragma once

#include "fs/directory_session.h"
#include "model/thumbnail_options.h"
#include "thumbnails/file_icon_cache.h"

namespace luciole {
  // Adapted from Noctalia's ThumbnailService acquire/release and worker publication model.
  // Cells borrow immutable prepared pixels; the shared texture cache owns upload and residency.
  class ThumbnailService {
  public:
    explicit ThumbnailService(ThumbnailPurpose purpose = ThumbnailPurpose::Thumbnail);
    ~ThumbnailService();
    ThumbnailService(const ThumbnailService&) = delete;
    ThumbnailService& operator=(const ThumbnailService&) = delete;
    std::shared_ptr<const FileIconImage> request(const DirectoryEntry& source, int pixels, bool visible = true);
    static int pixelSizeClass(int pixels, ThumbnailPurpose purpose);
    std::shared_ptr<const FileIconImage> prepared(const DirectoryEntry& source);
    bool touch(const std::shared_ptr<const FileIconImage>& image, bool visible);
    void synchronizeSources(const DirectorySnapshot& snapshot);
    void setOptions(ThumbnailOptions options);
    void setChangedCallback(std::function<void()> callback);
    void clearCache();
    void invalidate();
    void endFrame();
    bool clearing() const;
    const std::string& error() const;
    std::size_t reservedBytes() const;
    std::size_t activeJobs() const;
    std::size_t queuedJobs() const;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
