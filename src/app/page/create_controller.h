#pragma once

#include "browser/action_registry.h"
#include "fs/location.h"
#include "operations/create_service.h"
#include "operations/template_catalog.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/flex.h"
#include "wayland/popup_surface.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct KeyboardEvent;
struct wl_output;
struct wl_surface;
class AnimationManager;
class ContextMenuControl;
class LinkDialog;
class NameEditor;
class PageContext;
class RenderContext;
class ScrollView;
class WaylandConnection;

// New items: the New Document submenu of the file menu, the create file/folder dialog and the link dialog.
class CreateController {
public:
  CreateController(
      PageContext& page, files::CreateService& creator, files::TemplateCatalog& templates,
      WaylandConnection& connection, RenderContext& renderer, AnimationManager& animations
  );

  void openCreateItem(
      files::Location parent, files::CreateKind kind, std::optional<files::DocumentTemplate> source = std::nullopt
  );
  void openLinkDialog(const files::DirectoryEntry& target);
  // Opens the template submenu next to the context menu row given to attachDocumentMenu().
  void openDocumentMenu(const files::ActionRequest& request);
  // Service change: sync on the next frame.
  void markDirty() { m_dirty = true; }
  // Frame slot in BrowserPage::prepareFrame().
  void onFrame();

  // Context menu hooks for the New Document row and its submenu surface.
  void attachDocumentMenu(std::size_t row, const files::ActionRequest& request);
  // BrowserPage::closeMenu(): closes the submenu and forgets the row.
  void menuClosed();
  bool documentOwns(const wl_surface* surface) const;
  InputDispatcher& documentDispatcher() { return m_documentDispatcher; }
  void outputChanged(wl_output* output);
  // Handles keys while the submenu is open; false when it is not.
  bool documentKey(const KeyboardEvent& event);
  bool isDocumentRow(std::size_t index) const { return m_documentRow && index == *m_documentRow; }
  // Highlights the first template when the next submenu opens from the keyboard.
  void openedFromKeyboard() { m_documentKeyboard = true; }
  // Pointer activity on the submenu keeps it open.
  void documentPointer() { m_closeDocumentPending = false; }
  // Pointer entered or moved over the context menu.
  void menuHover();
  // The context menu scrolled.
  void menuScrolled();

private:
  bool canCreateItem() const;
  void closeDocumentMenu();
  void dismissDocumentMenu();
  void fillDocumentMenu(const files::ActionRequest& request, files::TemplateListing listing, bool loading = false);
  void submitCreateItem(std::string name);
  void sync();
  void closeCreation();

  PageContext& m_page;
  files::CreateService& m_creator;
  files::TemplateCatalog& m_templates;
  WaylandConnection& m_connection;
  RenderContext& m_renderer;
  AnimationManager& m_animations;
  std::unique_ptr<PopupSurface> m_documentSurface;
  std::unique_ptr<Flex> m_documentRoot;
  ContextMenuControl* m_documentMenu = nullptr;
  ScrollView* m_documentScroll = nullptr;
  InputDispatcher m_documentDispatcher;
  std::optional<std::size_t> m_documentRow;
  std::uint64_t m_documentGeneration = 0;
  bool m_closeDocumentPending = false;
  bool m_documentKeyboard = false;
  NameEditor* m_createEditor = nullptr;
  LinkDialog* m_linkDialog = nullptr;
  bool m_linkPreparing = false;
  std::optional<files::Location> m_createParent;
  bool m_creationPreparing = false;
  bool m_dirty = false;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
