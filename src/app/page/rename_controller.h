#pragma once

#include "config/state_store.h"
#include "fs/directory_session.h"

#include <memory>
#include <optional>
#include <string>

namespace files {
  class PinService;
} // namespace files

class NameDialog;
class OperationJob;
class PageContext;

// Single-item rename of a file or folder, and renaming a place in the sidebar.
class RenameController {
public:
  RenameController(PageContext& page, files::PinService& pins);

  void openFileRename(files::DirectoryEntry target);
  void openPlaceRename(const files::Bookmark& bookmark);
  // Owned rename job change: sync the editor on the next frame, or report a job whose editor closed.
  void changed(OperationJob& job);
  // Frame slot in BrowserPage::prepareFrame().
  void onFrame();
  // BrowserPage::prepareFrame() hook before syncBrowser(): closes the place editor once the label is saved.
  void syncPlaceRename();

private:
  bool canFileRename(const files::DirectoryEntry& entry) const;
  void submitFileRename(std::string name);
  void syncFileRename(const OperationJob& job);
  void submitPlaceRename(std::string label);

  PageContext& m_page;
  files::PinService& m_pins;
  // The editor's job: checks the target, then renames it.
  std::shared_ptr<OperationJob> m_job;
  NameDialog* m_fileRenameEditor = nullptr;
  std::optional<files::DirectoryEntry> m_fileRenameTarget;
  bool m_fileRenamePreparing = false;
  bool m_dirty = false;
  NameDialog* m_placeEditor = nullptr;
  std::optional<files::Bookmark> m_placeRenameTarget;
  std::optional<std::string> m_pendingPlaceLabel;
};
