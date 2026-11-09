#pragma once

#include "render/scene/node.h"

#include <memory>
#include <string>

namespace files {
  class PinService;
} // namespace files

class FileKeyboardController;
class Flex;
class Label;
class PageContext;
class SettingsController;
class TransferController;

// The status bar and the empty-folder notice: progress of the newest running job the page owns, the last action
// message, folder and selection counts, free space, and the notice shown in place of an empty file list.
class StatusBarController {
public:
  StatusBarController(
      PageContext& page, files::PinService& pins, TransferController& transferController, SettingsController& settings,
      FileKeyboardController& keys
  );

  // The last action message; shown unless an operation runs.
  void setMessage(std::string message) { m_actionError = std::move(message); }
  void clearMessage() { m_actionError.clear(); }

  // Scene: the status row at the bottom of the page and the notice inside the file pane.
  std::unique_ptr<Flex> build();
  std::unique_ptr<Flex> buildNotice();

  // BrowserPage::syncBrowser(): status text, free space, operation buttons and the notice for an empty list.
  void sync(bool miller);
  // Layout hook in BrowserPage::prepareFrame(): free space shows only in a pane at least that wide.
  void layout(float paneWidth);
  // True while the notice replaces the file list.
  bool noticeShown() const;
  // Scene rectangle of the status text, where the input method places its candidates.
  LayoutRect statusRect() const;

private:
  std::string operationStatus() const;

  PageContext& m_page;
  files::PinService& m_pins;
  TransferController& m_transferController;
  SettingsController& m_settings;
  FileKeyboardController& m_keys;
  std::string m_actionError;
  Label* m_status = nullptr;
  Label* m_freeSpace = nullptr;
  bool m_freeSpaceAvailable = false;
  Label* m_notice = nullptr;
  Flex* m_noticeArea = nullptr;
};
