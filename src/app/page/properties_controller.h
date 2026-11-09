#pragma once

#include <cstddef>
#include <vector>

namespace files {
  class FileIconCache;
  class PermissionService;
  struct DirectoryEntry;
} // namespace files

struct KeyboardEvent;
class AclEditor;
class BulkAclEditor;
class Button;
class Flex;
class Label;
class PageContext;
class PermissionEditor;
class PermissionResultsView;
class ScrollView;
class Segmented;

// The Properties dialog: general facts, and for up to 4096 items the permission and ACL editors with their results.
class PropertiesController {
public:
  PropertiesController(PageContext& page, files::PermissionService& permissions, files::FileIconCache& icons);

  // `captureCurrentLocation` reads the permissions of the first target's location instead of its listed entry.
  void show(const std::vector<files::DirectoryEntry>& targets, bool captureCurrentLocation = false);
  // Service change: sync on the next frame.
  void markDirty() { m_dirty = true; }
  // Operation progress tick: keep syncing while permissions are read or applied.
  void progressTick();
  // Frame slot in BrowserPage::prepareFrame().
  void onFrame();
  // BrowserPage::syncBrowser() hook: Apply is enabled only while no file operation runs.
  void updateApply();
  // Keyboard hook in BrowserPage::keyboard() while a popup is shown: keyboard navigation in the results list.
  bool resultsKey(const KeyboardEvent& event);
  // Menu popup hook: keeps the editable dialog at its fixed height.
  void layoutDialog(float width);

private:
  struct Body;

  Body buildBody(
      const std::vector<files::DirectoryEntry>& targets, float width, bool editable, bool multiple, bool folders,
      bool withResults
  );
  void showAdvanced(bool advanced, bool focus = false);
  bool canApply() const;
  void sync();

  PageContext& m_page;
  files::PermissionService& m_permissions;
  files::FileIconCache& m_icons;
  PermissionEditor* m_permissionEditor = nullptr;
  AclEditor* m_aclEditor = nullptr;
  BulkAclEditor* m_bulkAclEditor = nullptr;
  Flex *m_permissionBasic = nullptr, *m_permissionAdvanced = nullptr;
  Button *m_permissionAdvancedOpen = nullptr, *m_permissionAdvancedBack = nullptr;
  bool m_aclActive = false;
  std::size_t m_permissionResultTab = 2;
  PermissionResultsView* m_permissionResults = nullptr;
  Button *m_permissionReport = nullptr, *m_permissionCancel = nullptr;
  Label* m_permissionStatus = nullptr;
  Segmented* m_permissionTabs = nullptr;
  ScrollView* m_permissionScroll = nullptr;
  Button* m_permissionSave = nullptr;
  bool m_dirty = false;
};
