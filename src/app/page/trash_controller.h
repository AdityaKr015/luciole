#pragma once

#include "fs/directory_session.h"

#include <optional>
#include <vector>

namespace files {
  class EmptyTrashService;
  class RestoreService;
  class TrashService;
} // namespace files

class Button;
class Label;
class PageContext;

// Moving items to Trash, emptying Trash or deleting from it, and reporting restored items.
class TrashController {
public:
  TrashController(
      PageContext& page, files::TrashService& trash, files::EmptyTrashService& emptyTrash,
      files::RestoreService& restore
  );

  void trashItems(const std::vector<files::DirectoryEntry>& targets);
  void restore(const std::vector<files::DirectoryEntry>& targets);
  // Empties Trash, or deletes `selection` from it, after the user confirms.
  void openEmptyTrash(std::optional<std::vector<files::DirectoryEntry>> selection = std::nullopt);
  // Service changes: sync on the next frame.
  void markTrashDirty() { m_trashDirty = true; }
  void markEmptyTrashDirty() { m_emptyTrashDirty = true; }
  // RestoreService change: reports the outcome once restoring finishes.
  void restoreChanged();
  // Frame slot in BrowserPage::prepareFrame() and pumpJobs().
  void onFrame();
  // Menu popup hook: fits the confirmation dialog's height to its content.
  void layoutDialog();
  // True when the last deletion targets selected items instead of the whole Trash.
  bool selectedDelete() const { return m_selectedDelete; }

private:
  bool canTrashItems(const std::vector<files::DirectoryEntry>& targets) const;
  void confirmEmptyTrash();
  void syncTrash();
  void syncEmptyTrash();

  PageContext& m_page;
  files::TrashService& m_trash;
  files::EmptyTrashService& m_emptyTrash;
  files::RestoreService& m_restore;
  bool m_trashDirty = false;
  bool m_emptyTrashDirty = false;
  bool m_emptyTrashDialog = false;
  bool m_selectedDelete = false;
  Label* m_emptyTrashMessage = nullptr;
  Label* m_emptyTrashSize = nullptr;
  Button* m_emptyTrashConfirm = nullptr;
};
