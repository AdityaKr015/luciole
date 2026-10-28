#pragma once

#include "browser/file_icon_cache.h"
#include "render/core/texture_handle.h"
#include "render/scene/node.h"
#include "ui/palette.h"

class ImageNode;
class TextureManager;

namespace files {
  class FileIcon final : public Node {
  public:
    FileIcon(FileIconCache& cache, float size);
    ~FileIcon() override;
    void bind(const FileIconSource& source, ColorSpec foreground);
    void setIconSize(float size);

  protected:
    void doLayout(Renderer& renderer) override;
    LayoutSize doMeasure(Renderer&, const LayoutConstraints&) override;
    void doInvalidateGpuResources(Renderer&) override;

  private:
    FileIconCache& m_cache;
    float m_size;
    FileIconSource m_source;
    ColorSpec m_foreground = colorSpecFromRole(ColorRole::OnSurface);
    ImageNode* m_image = nullptr;
    TextureManager* m_textures = nullptr;
    TextureHandle m_texture{};
    std::shared_ptr<const FileIconImage> m_asset;
  };
} // namespace files
