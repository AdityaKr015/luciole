#pragma once

#include "thumbnails/preview_service.h"
#include "ui/browser/file_icon.h"
#include "ui/controls/flex.h"

class Label;
class Button;
class ScrollView;
class FileMetadataView;
class FilePreviewView final : public Flex {
public:
  FilePreviewView(
      files::FileIconCache& icons, std::function<void()> close, std::function<void()> open,
      std::function<void()> openWith
  );
  void sync(
      const files::PreviewSnapshot& snapshot, float width, float height, std::string_view typeLabel = {},
      bool quick = false
  );
  void clear(Renderer& renderer);

private:
  files::FileIcon* m_image;
  Label *m_heading, *m_name, *m_status, *m_text;
  FileMetadataView* m_metadata;
  Button *m_open, *m_openWith;
  ScrollView* m_scroll;
  std::string m_location;
};
