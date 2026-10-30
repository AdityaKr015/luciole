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
  m_thumbnailSource.reset();
  m_thumbnail.reset();
  m_source = source;
  m_foreground = foreground;
  markLayoutDirty();
}
void files::FileIcon::bindFile(const DirectoryEntry& entry, ColorSpec foreground) {
  if (!m_thumbnailSource
      || entry.id != m_thumbnailSource->id
      || entry.location != m_thumbnailSource->location
      || entry.etag != m_thumbnailSource->etag
      || entry.size != m_thumbnailSource->size
      || entry.modifiedSeconds != m_thumbnailSource->modifiedSeconds)
    m_thumbnail.reset();
  m_thumbnailSource = entry;
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
  m_thumbnailSource.reset();
  m_thumbnail.reset();
  m_asset.reset();
  if (m_texture.valid())
    renderer.textureManager().unload(m_texture);
  m_texture = {};
  m_image->setTextureId({});
}
void files::FileIcon::setIconSize(float size) {
  if (m_size != size) {
    m_size = size;
    markLayoutDirty();
  }
}
LayoutSize files::FileIcon::doMeasure(Renderer&, const LayoutConstraints&) { return {m_size, m_size}; }
void files::FileIcon::doLayout(Renderer& renderer) {
  const int pixels = static_cast<int>(std::ceil(m_size * renderer.renderScale()));
  if (m_thumbnails && m_thumbnailSource) {
    auto image = m_thumbnails->request(*m_thumbnailSource, pixels, m_thumbnailVisible);
    m_thumbnail = std::move(image);
  }
  const auto asset =
      m_thumbnailVisible && m_thumbnail && m_thumbnail->pixels ? m_thumbnail : m_cache.request(m_source, pixels);
  m_textures = &renderer.textureManager();
  if (asset != m_asset) {
    if (m_texture.valid()) {
      m_textures->unload(m_texture);
    }
    m_asset = asset;
  }
  if (!m_texture.valid() && m_asset && m_asset->pixels) {
    const auto& pixels = *m_asset->pixels;
    m_texture = m_textures->loadFromRgba(pixels.rgba.data(), pixels.width, pixels.height);
  }
  m_image->setTextureId(m_texture.id);
  m_image->setTextureSize(m_texture.width, m_texture.height);
  const bool symbolic = m_asset && m_asset->symbolic;
  m_image->setAlphaMaskTint(symbolic);
  m_image->setTint(symbolic ? resolveColorSpec(m_foreground) : Color{1, 1, 1, 1});
  m_image->setSize(width(), height());
}
void files::FileIcon::doInvalidateGpuResources(Renderer&) {
  m_texture = {};
  m_image->setTextureId({});
  markLayoutDirty();
}
