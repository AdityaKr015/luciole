#pragma once

#include <memory>

namespace files {
  class FileIconCache;
  class FileLauncher;
  struct DirectoryEntry;
} // namespace files

class Flex;
class InputArea;
class PageContext;
class ScrollView;

// The Open With dialog: lists the applications that can open a file and opens it with the chosen one.
class ApplicationChooserController {
public:
  ApplicationChooserController(PageContext& page, files::FileLauncher& launcher, files::FileIconCache& icons);

  void show(const files::DirectoryEntry& entry);
  // Focus hook of the popup dispatcher: scrolls the focused control into view.
  void revealFocus(InputArea* focused);

private:
  PageContext& m_page;
  files::FileLauncher& m_launcher;
  files::FileIconCache& m_icons;
  Flex* m_container = nullptr;
  ScrollView* m_scroll = nullptr;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
