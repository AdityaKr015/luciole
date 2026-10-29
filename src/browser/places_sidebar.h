#pragma once

#include "browser/file_icon.h"
#include "browser/places.h"
#include "ui/controls/button.h"
#include "ui/controls/scroll_view.h"

namespace files {
  class PlacesSidebar final : public Flex {
  public:
    PlacesSidebar(PinService& pins, FileIconCache& icons);
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

  protected:
    void doLayout(Renderer& renderer) override;
    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
    void doArrange(Renderer& renderer, const LayoutRect& rect) override;

  private:
    struct Row {
      Bookmark bookmark;
      std::string iconName;
      std::size_t pinIndex = 0;
      Button* button = nullptr;
      FileIcon* icon = nullptr;
    };
    void rebuild();
    void addRow(const Bookmark& bookmark, const std::string& icon, std::size_t index);
    PinService& m_pins;
    FileIconCache& m_icons;
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
