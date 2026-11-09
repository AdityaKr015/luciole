#pragma once

#include "model/details_columns.h"
#include "model/directory_options.h"
#include "render/scene/node.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace files {
  class FileViewAdapter;
  class ViewPreferenceWriter;
  struct BrowserSettings;
} // namespace files

struct KeyboardEvent;
struct PointerEvent;
class Box;
class Button;
class Flex;
class InputArea;
class InputDispatcher;
class MenuController;
class PageContext;
class Renderer;
class Scrollbar;
class Separator;

// The details view header: column buttons that sort, resize handles, dragging a column to reorder it, the column
// drop line, horizontal scrolling of wide columns, and the saved column visibility, order and widths.
class DetailsHeaderController {
public:
  DetailsHeaderController(
      PageContext& page, InputDispatcher& dispatcher, files::ViewPreferenceWriter& preferences,
      files::FileViewAdapter& listAdapter, files::FileViewAdapter& gridAdapter, MenuController& menus
  );

  // Takes the column settings of a new page.
  void load(const files::BrowserSettings& settings);
  // Takes the column settings that changed; `force` takes them all.
  void applySettings(const files::BrowserSettings& before, const files::BrowserSettings& settings, bool force);

  const files::DetailsColumnVisibility& columnVisibility() const { return m_columnVisibility; }
  const files::DetailsColumnOrder& columnOrder() const { return m_columnOrder; }
  const files::DetailsColumnWidths& columnWidths() const { return m_columnWidths; }
  // The column order with `column` moved one place in `direction`; empty when it cannot move.
  std::optional<files::DetailsColumnOrder> movedColumnOrder(files::DetailsColumn column, int direction) const;
  void setDetailsColumnVisibility(files::DetailsColumnVisibility visibility);
  void setDetailsColumnOrder(files::DetailsColumnOrder order);
  void setDetailsColumnWidths(files::DetailsColumnWidths widths);

  // Horizontal scroll offset of the details columns.
  float offset() const { return m_detailsOffset; }
  void setOffset(float offset) { m_detailsOffset = offset; }
  // True while a column is dragged or resized.
  bool gestureActive() const { return m_columnDrag || m_columnResize; }
  // Releases the pointer capture of a column drag or resize in progress.
  void cancelGesture();

  // Scene: the header row above the file list, the horizontal scrollbar below it, and, once the page root
  // exists, the resize handles, the header press hooks and the drop line on `root`.
  std::unique_ptr<Flex> buildHeader();
  std::unique_ptr<Box> buildScrollbar();
  void attach(Node& root);

  // Focus hook of the page dispatcher: scrolls a focused header into view.
  void focusChanged(InputArea* focused);
  // BrowserPage::syncBrowser(): sort indicators, header visibility and the list adapter's columns.
  void sync(const files::SortOptions& sort);
  // Layout hooks in BrowserPage::prepareFrame(), in this order.
  // Moves the header buttons into the saved order; true when it changed.
  bool applyOrder();
  // Sizes the columns for the viewport; false in the Miller view, which has no header. Arrange the page next.
  bool layoutColumns(bool miller);
  // Places the header row, its scrollbar and resize handles in the arranged page.
  void layoutHeader(Renderer& renderer);
  // True once after a change that needs the pointer hover refreshed.
  bool takeHoverDirty() { return std::exchange(m_columnHoverDirty, false); }
  void layoutDrag(Renderer& renderer);

  // Pointer hooks in BrowserPage::pointer() for the page surface.
  // Finishes a column drag on the left button release.
  bool release(const PointerEvent& event, float x, float y);
  // Opens the columns menu on a right press over the header.
  bool contextPress(const PointerEvent& event, float x, float y);
  // Scrolls the columns horizontally unless a column gesture runs.
  void scrollWheel(const PointerEvent& event);
  bool headerContains(float x, float y) const;
  // Keyboard hooks in BrowserPage::keyboard() while no text field is focused.
  // Ctrl+Left/Right resizes the focused header.
  bool widthKey(const KeyboardEvent& event);
  // The menu key opens the columns menu for the focused header.
  bool menuKey(const KeyboardEvent& event);

private:
  struct ColumnResize {
    std::size_t index;
    float pressX;
    float width;
    files::DetailsColumnWidths before;
    float offset;
    int nameWidth;
  };
  struct ColumnDrag {
    files::DetailsColumn source;
    float pressX;
    float pressY;
    std::uint32_t cursor;
    bool active = false;
  };
  struct ColumnDrop {
    files::DetailsColumnOrder order;
    LayoutRect marker;
  };

  Button* widthHeader(std::size_t index) const;
  float headerWidth(std::size_t index) const;
  Button* columnHeader(files::DetailsColumn column) const;
  void sortBy(files::SortField field);
  void cancelColumnResize();
  void scrollDetails(float offset);
  std::optional<ColumnDrop> columnDropAt(float x, float y) const;
  void cancelColumnDrag();
  void updateColumnDrag();
  void finishColumnDrag(float x, float y);

  PageContext& m_page;
  InputDispatcher& m_dispatcher;
  files::ViewPreferenceWriter& m_preferences;
  files::FileViewAdapter& m_listAdapter;
  files::FileViewAdapter& m_gridAdapter;
  MenuController& m_menus;
  Flex* m_listHeader = nullptr;
  Button* m_nameHeader = nullptr;
  std::array<Button*, files::kDetailsColumnCount> m_headers{};
  files::DetailsColumns m_detailsColumns;
  files::DetailsColumnWidths m_columnWidths = files::kDefaultDetailsColumnWidths;
  std::optional<ColumnResize> m_columnResize;
  std::array<InputArea*, files::kDetailsColumnCount + 1> m_columnResizeHandles{};
  std::array<Separator*, files::kDetailsColumnCount + 1> m_columnResizeLines{};
  Flex* m_detailsHeaderViewport = nullptr;
  Box* m_detailsScrollHost = nullptr;
  Scrollbar* m_detailsScrollbar = nullptr;
  float m_detailsOffset = 0.0F;
  float m_viewport = 0.0F;
  Button* m_revealDetailsHeader = nullptr;
  InputArea* m_lastDetailsFocus = nullptr;
  files::DetailsColumnVisibility m_columnVisibility;
  files::DetailsColumnOrder m_columnOrder = files::kDefaultDetailsColumnOrder;
  files::DetailsColumnOrder m_appliedColumnOrder = files::kDefaultDetailsColumnOrder;
  std::optional<ColumnDrag> m_columnDrag;
  bool m_columnHoverDirty = false;
  Box* m_columnDropLine = nullptr;
  Flex* m_detailsHeaderRow = nullptr;
};
