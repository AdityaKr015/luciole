#pragma once

#include "browser/action_registry.h"
#include "fs/location.h"
#include "operations/create_service.h"
#include "operations/template_catalog.h"

#include <memory>
#include <optional>
#include <string>

class LinkDialog;
class NameEditor;
class OperationJob;
class PageContext;
class SubmenuController;

// New items: the New Document submenu of the file menu, the create file/folder dialog and the link dialog.
class CreateController {
public:
  CreateController(PageContext& page, files::TemplateCatalog& templates, SubmenuController& submenus);

  void openCreateItem(
      files::Location parent, files::CreateKind kind, std::optional<files::DocumentTemplate> source = std::nullopt
  );
  void openLinkDialog(const files::DirectoryEntry& target);
  // Fills the opening New Document submenu with the templates, which load meanwhile.
  void openDocumentMenu(const files::ActionRequest& request);
  // The New Document submenu closed.
  void documentMenuClosed() { m_templates.cancel(); }
  // Owned creation job change: sync the dialog on the next frame, or report a job whose dialog closed.
  void changed(OperationJob& job);
  // Frame slot in BrowserPage::prepareFrame().
  void onFrame();

private:
  bool canCreateItem() const;
  void showDocumentMenu(const files::ActionRequest& request, files::TemplateListing listing, bool loading = false);
  void submitCreateItem(std::string name);
  void sync(const OperationJob& job);
  void closeCreation();

  PageContext& m_page;
  // The create or link dialog's job: checks the destination, then creates the item.
  std::shared_ptr<OperationJob> m_job;
  files::TemplateCatalog& m_templates;
  SubmenuController& m_submenus;
  NameEditor* m_createEditor = nullptr;
  LinkDialog* m_linkDialog = nullptr;
  bool m_linkPreparing = false;
  std::optional<files::Location> m_createParent;
  bool m_creationPreparing = false;
  bool m_dirty = false;
};
