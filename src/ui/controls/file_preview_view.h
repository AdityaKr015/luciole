#pragma once

#include "browser/file_icon.h"
#include "browser/preview_service.h"
#include "ui/controls/flex.h"

class Label;
class Button;
class ScrollView;
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
  Label *m_heading, *m_name, *m_metadata, *m_status, *m_text;
  Button *m_open, *m_openWith;
  ScrollView* m_scroll;
  Flex* m_actions;
  std::string m_location;
};
