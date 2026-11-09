#pragma once

#include "browser/action_registry.h"
#include "fs/location.h"
#include "model/details_columns.h"
#include "render/scene/node.h"
#include "ui/controls/context_menu.h"

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

namespace files {
  class PathCompletion;
  class PinService;
  class TerminalLauncher;
  class TransferService;
  struct Bookmark;
} // namespace files

struct KeyboardEvent;
class CreateController;
class DetailsHeaderController;
class LocationBarController;
class PageContext;
class PropertiesController;
class RenameController;
class ScrollView;

// Context menus: the file, place, view, sort, selection, columns and breadcrumb menus, and keyboard navigation of
// whichever context menu the popup shows.
class MenuController {
public:
  MenuController(
      PageContext& page, files::PinService& pins, files::TransferService& transfers, files::TerminalLauncher& terminal,
      files::PathCompletion& siblingCompletion, CreateController& creator, RenameController& renamer,
      PropertiesController& properties, DetailsHeaderController& columns, LocationBarController& locationBar
  );

  // Shows `entries` on the popup; activating an entry closes the menu and then calls `action` with its id.
  void showContextMenu(
      std::vector<ContextMenuControlEntry> entries, std::function<void(int)> action, LayoutRect anchor,
      bool alignLeft = false, float preferredWidth = 264.0F
  );
  void openFileMenu(std::optional<files::EntryId> id, float x, float y);
  void openPlaceMenu(const files::Bookmark& bookmark, float x, float y);
  void openColumnsMenu(const LayoutRect& anchor, std::optional<files::DetailsColumn> column = std::nullopt);
  void openBreadcrumbMenu(const files::Location& target, LayoutRect anchor);
  void openMenu();
  void openSortMenu(std::optional<LayoutRect> anchor = std::nullopt, bool alignLeft = true);
  void openSelectionMenu();
  // Scrolls the highlighted row into view.
  void revealHighlight();
  // Keyboard hook in BrowserPage::keyboard(): handles every key while the popup is shown.
  void key(const KeyboardEvent& event);

  // The context menu on the popup; null while the popup shows a dialog or nothing.
  ContextMenuControl* menu() const { return m_menu; }
  // BrowserPage::showPopup(): the popup now shows `menu`, or a dialog when null.
  void setMenu(ContextMenuControl* menu) { m_menu = menu; }
  // BrowserPage::closeMenu(): forgets the menu widgets.
  void menuClosed();

private:
  ContextMenuControlEntry
  actionEntry(files::BrowserAction action, const files::ActionRequest& request, bool radio = false) const;
  void showActionMenu(
      std::initializer_list<std::optional<files::BrowserAction>> actions, bool radio = false, bool back = false,
      std::optional<LayoutRect> anchor = std::nullopt, bool alignLeft = true
  );

  PageContext& m_page;
  files::PinService& m_pins;
  files::TransferService& m_transfers;
  files::TerminalLauncher& m_terminal;
  files::PathCompletion& m_siblingCompletion;
  CreateController& m_creator;
  RenameController& m_renamer;
  PropertiesController& m_properties;
  DetailsHeaderController& m_columns;
  LocationBarController& m_locationBar;
  ContextMenuControl* m_menu = nullptr;
  ScrollView* m_scroll = nullptr;
  std::shared_ptr<int> m_lifetime = std::make_shared<int>(0);
};
