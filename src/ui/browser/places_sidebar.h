#pragma once

#include "browser/places.h"
#include "ui/controls/button.h"
#include "ui/controls/scroll_view.h"

namespace files {
  class SidebarTreeView;
  class PlacesSidebar final : public Flex {
  public:
    explicit PlacesSidebar(PinService& pins);
    void setTreeEnabled(bool enabled);
    void setShowHidden(bool showHidden);
    void setChangedCallback(std::function<void()> callback) { m_changed = std::move(callback); }
    void invalidate();
    void sync(std::optional<Location> current);
    void cancelDragCandidate();
    std::optional<std::size_t> dropSlot(float x, float y) const;
    LayoutRect dropLine(std::size_t slot) const;
    void dragScroll(float delta);
    float dragScrollSpeed(float x, float y) const;
    void setOpenCallback(std::function<void(const Location&)> callback);
    void setContextCallback(std::function<void(const Bookmark&, float, float)> callback);
    void setDragCallback(std::function<void(const Bookmark&)> callback);
    void setRenameCallback(std::function<void(const Bookmark&)> callback);
    InputArea* focusArea(const Location& location) const;
    std::optional<Bookmark> folderAt(float x, float y) const;
    std::optional<LayoutRect> folderRect(const Location& location) const;

  protected:
    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
    void doArrange(Renderer& renderer, const LayoutRect& rect) override;

  private:
    struct Row {
      Bookmark bookmark;
      std::size_t pinIndex = 0;
      Button* button = nullptr;
    };
    void rebuild();
    void addRow(const Bookmark& bookmark, const std::string& glyph, std::size_t index);
    PinService& m_pins;
    SidebarTreeView* m_tree = nullptr;
    bool m_treeEnabled = false, m_showHidden = false;
    std::function<void()> m_changed;
    ScrollView* m_scroll = nullptr;
    Label* m_heading = nullptr;
    std::vector<Row> m_rows;
    std::optional<Location> m_current;
    std::optional<Bookmark> m_dragCandidate;
    float m_pressX = 0;
    float m_pressY = 0;
    bool m_dirty = true;
    bool m_dragged = false;
    std::function<void(const Location&)> m_open;
    std::function<void(const Bookmark&, float, float)> m_context;
    std::function<void(const Bookmark&)> m_drag;
    std::function<void(const Bookmark&)> m_rename;
  };
} // namespace files
