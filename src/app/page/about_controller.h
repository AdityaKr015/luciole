#pragma once

#include <string>

namespace files {
  class FileLauncher;
} // namespace files

class InputArea;
class Label;
class PageContext;
class ScrollView;

// The About dialog.
class AboutController {
public:
  AboutController(PageContext& page, files::FileLauncher& launcher);

  void open();
  // Launcher change: shows why a link did not open, or hides the message once one did.
  void launcherChanged(const std::string& error);
  // Focus hook of the popup dispatcher: scrolls the focused control into view.
  void revealFocus(InputArea* focused);

private:
  PageContext& m_page;
  files::FileLauncher& m_launcher;
  ScrollView* m_scroll = nullptr;
  Label* m_message = nullptr;
};
