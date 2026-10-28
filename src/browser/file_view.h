#pragma once

#include "browser/browser_controller.h"
#include "browser/file_icon_cache.h"
#include "ui/controls/virtual_grid_view.h"

#include <chrono>

namespace files {
  class FileViewAdapter final : public VirtualGridAdapter {
  public:
    FileViewAdapter(
        BrowserController& browser, FileIconCache& icons, ViewMode mode, std::function<std::uint32_t()> modifiers,
        std::function<bool()> focused
    );
    [[nodiscard]] std::size_t itemCount() const override;
    [[nodiscard]] std::unique_ptr<Node> createTile() override;
    void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override;
    bool onPointerPress(std::size_t index, float, float, float, float) override;
    bool onPointerDrag(std::optional<std::size_t>, float, float, float, float) override;
    PointerReleaseResult onPointerRelease(std::optional<std::size_t> index) override;
    void onPointerCancel() override;
    bool onBackgroundPointerPress() override;
    bool onAreaSelection(const std::vector<std::size_t>& indices) override;
    bool areaSelectionCurrent() const override;
    bool itemHitTest(std::size_t, float x, float y, float width, float height) const override;
    void onActivate(std::size_t index) override;
    void setGridIconSize(float size) { m_gridIconSize = size; }

  private:
    BrowserController& m_browser;
    FileIconCache& m_icons;
    const ViewMode m_mode;
    std::function<std::uint32_t()> m_modifiers;
    std::function<bool()> m_focused;
    std::optional<EntryId> m_pressed;
    std::optional<EntryId> m_lastClick;
    std::chrono::steady_clock::time_point m_lastClickTime;
    bool m_dragged = false;
    bool m_plainClick = false;
    float m_gridIconSize = 48.0F;
    std::vector<EntryId> m_areaRows;
    std::unordered_set<EntryId> m_areaBaseline;
    std::uint64_t m_areaGeneration = 0;
    bool m_areaSelection = false;
    bool m_areaToggle = false;
  };
} // namespace files
