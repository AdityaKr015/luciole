#include "browser/file_icon.h"

#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"

#include <algorithm>
#include <cmath>

files::FileIcon::FileIcon(FileIconCache& cache, float size, ThumbnailService* thumbnails)
    : m_cache(cache), m_thumbnails(thumbnails), m_size(size) {
  m_image = static_cast<ImageNode*>(addChild(std::make_unique<ImageNode>()));
  m_image->setFitMode(ImageFitMode::Contain);
  setHitTestVisible(false);
}
files::FileIcon::~FileIcon() {
  if (m_textures != nullptr && m_texture.valid()) {
    m_textures->unload(m_texture);
  }
}
void files::FileIcon::bind(const FileIconSource& source, ColorSpec foreground) {
  if (!m_thumbnailSource && source == m_source && foreground == m_foreground)
    return;
  m_thumbnailSource.reset();
  m_thumbnail.reset();
  m_thumbnailPixels = 0;
  m_source = source;
  m_foreground = foreground;
  markLayoutDirty();
}
void files::FileIcon::bindFile(const DirectoryEntry& entry, ColorSpec foreground) {
  const bool changed = !m_thumbnailSource
      || entry.id != m_thumbnailSource->id
      || entry.location != m_thumbnailSource->location
      || entry.etag != m_thumbnailSource->etag
      || entry.size != m_thumbnailSource->size
      || entry.modifiedSeconds != m_thumbnailSource->modifiedSeconds
      || entry.contentRevision != m_thumbnailSource->contentRevision
      || entry.identity != m_thumbnailSource->identity
      || entry.type != m_thumbnailSource->type
      || entry.symlink != m_thumbnailSource->symlink
      || entry.contentType != m_thumbnailSource->contentType;
  if (!changed && entry.icon == m_source && foreground == m_foreground)
    return;
  if (changed) {
    m_thumbnail.reset();
    m_thumbnailPixels = 0;
    m_thumbnailSource = entry;
  }
  m_source = entry.icon;
  m_foreground = foreground;
  markLayoutDirty();
}
void files::FileIcon::setThumbnailVisible(bool visible) {
  if (m_thumbnailVisible != visible) {
    m_thumbnailVisible = visible;
    markLayoutDirty();
  }
}
void files::FileIcon::releaseFile(Renderer& renderer) {
  const bool changed = m_prepared || m_thumbnailSource || m_thumbnail || m_asset || m_texture.valid();
  m_prepared.reset();
  m_thumbnailSource.reset();
  m_thumbnail.reset();
  m_thumbnailPixels = 0;
  m_asset.reset();
  if (m_texture.valid())
    renderer.textureManager().unload(m_texture);
  m_texture = {};
  m_image->setTextureId({});
  if (changed)
    markLayoutDirty();
}
void files::FileIcon::setIconSize(float size) {
  if (m_size != size) {
    m_size = size;
    markLayoutDirty();
  }
}
void files::FileIcon::setPreparedImage(std::shared_ptr<const FileIconImage> image) {
  if (m_prepared != image) {
    m_prepared = std::move(image);
    markLayoutDirty();
  }
}
void files::FileIcon::setPreviewHeight(float height) {
  if (m_previewHeight != height) {
    m_previewHeight = height;
    markLayoutDirty();
  }
}
LayoutSize files::FileIcon::doMeasure(Renderer&, const LayoutConstraints&) {
  return {m_size, m_previewHeight > 0 ? m_previewHeight : m_size};
}
void files::FileIcon::doLayout(Renderer& renderer) {
  const int pixels = static_cast<int>(std::ceil(m_size * renderer.renderScale()));
  if (m_thumbnails
      && m_thumbnailSource
      && m_thumbnailSource->type == G_FILE_TYPE_REGULAR
      && !m_thumbnailSource->symlink
      && m_thumbnailSource->modifiedSeconds) {
    if (pixels != m_thumbnailPixels || !m_thumbnails->touch(m_thumbnail, m_thumbnailVisible)) {
      m_thumbnail = m_thumbnails->request(*m_thumbnailSource, pixels, m_thumbnailVisible);
      m_thumbnailPixels = pixels;
    }
  }
  const auto asset = m_prepared && m_prepared->pixels            ? m_prepared
      : m_thumbnailVisible && m_thumbnail && m_thumbnail->pixels ? m_thumbnail
                                                                 : m_cache.request(m_source, pixels);
  m_textures = &renderer.textureManager();
  if (asset != m_asset) {
    if (m_texture.valid()) {
      m_textures->unload(m_texture);
    }
    m_asset = asset;
  }
  if (m_thumbnailVisible && !m_texture.valid() && m_asset && m_asset->pixels) {
    const auto& pixels = *m_asset->pixels;
    m_texture = m_textures->loadFromRgba(pixels.rgba.data(), pixels.width, pixels.height, m_previewHeight > 0);
  }
  m_image->setTextureId(m_texture.id);
  m_image->setTextureSize(m_texture.width, m_texture.height);
  const bool symbolic = m_asset && m_asset->symbolic;
  m_image->setAlphaMaskTint(symbolic);
  m_image->setTint(symbolic ? resolveColorSpec(m_foreground) : Color{1, 1, 1, 1});
  m_image->setPosition(0, 0);
  m_image->setSize(width(), height());
}
void files::FileIcon::doInvalidateGpuResources(Renderer&) {
  m_texture = {};
  m_image->setTextureId({});
  markLayoutDirty();
}
