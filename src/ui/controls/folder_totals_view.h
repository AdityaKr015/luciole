#pragma once

#include "fs/folder_totals.h"
#include "ui/controls/flex.h"

class Button;
class Label;

// Properties-only recursive inspection, using the shared controls and a worker service.
class FolderTotalsView final : public Flex {
public:
  FolderTotalsView(std::span<const files::DirectoryEntry> targets, std::function<void()> requestSync);
  void sync(float width);

private:
  files::FolderTotalsService m_service;
  Label *m_status = nullptr, *m_counts = nullptr, *m_note = nullptr;
  Button* m_stop = nullptr;
};
