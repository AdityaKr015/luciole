#pragma once

#include "config/state_store.h"
#include "fs/directory_session.h"

#include <optional>
#include <string>

namespace files {
  class PinService;
  class RenameService;
} // namespace files

class NameEditor;
class PageContext;

// Single-item rename of a file or folder, and renaming a place in the sidebar.
class RenameController {
public:
  RenameController(PageContext& page, files::RenameService& renamer, files::PinService& pins);

  void openFileRename(files::DirectoryEntry target);
  void openPlaceRename(const files::Bookmark& bookmark);
  // Service change: sync on the next frame.
  void markDirty() { m_dirty = true; }
  // Frame slot in BrowserPage::prepareFrame().
  void onFrame();
  // BrowserPage::prepareFrame() hook before syncBrowser(): closes the place editor once the label is saved.
  void syncPlaceRename();

private:
  bool canFileRename(const files::DirectoryEntry& entry) const;
  void submitFileRename(std::string name);
  void syncFileRename();
  void submitPlaceRename(std::string label);

  PageContext& m_page;
  files::RenameService& m_renamer;
  files::PinService& m_pins;
  NameEditor* m_fileRenameEditor = nullptr;
  std::optional<files::DirectoryEntry> m_fileRenameTarget;
  bool m_fileRenamePreparing = false;
  bool m_dirty = false;
  NameEditor* m_placeEditor = nullptr;
  std::optional<files::Bookmark> m_placeRenameTarget;
  std::optional<std::string> m_pendingPlaceLabel;
};
