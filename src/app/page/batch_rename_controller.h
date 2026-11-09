#pragma once

#include "fs/location.h"

#include <cstdint>
#include <vector>

namespace files {
  class BatchRenameService;
  struct DirectoryEntry;
} // namespace files

class BatchRenameDialog;
class PageContext;
class Renderer;
class VirtualGridView;

// Batch rename dialog: preview, apply and report a rename of several items, then select the results.
class BatchRenameController {
public:
  BatchRenameController(PageContext& page, files::BatchRenameService& service);

  void open(const std::vector<files::DirectoryEntry>& targets);
  // Service change: sync on the next frame.
  void markDirty() { m_dirty = true; }
  // Operation progress tick: keep syncing while the rename applies.
  void progressTick();
  // Frame slot in BrowserPage::prepareFrame().
  void onFrame();

  // Menu popup hooks.
  void pointer(float x, float y, bool hover);
  void layoutDialog(float width);
  void syncTooltip(Renderer& renderer);
  // File list hook in BrowserPage::syncBrowser(): select the renamed items once the folder reloads.
  void revealRenamed(VirtualGridView& list);

private:
  void sync();

  PageContext& m_page;
  files::BatchRenameService& m_service;
  BatchRenameDialog* m_dialog = nullptr;
  bool m_dirty = false;
  bool m_reported = false;
  std::vector<files::Location> m_reveal;
  std::uint64_t m_revealContext = 0;
};
