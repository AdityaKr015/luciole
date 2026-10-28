#pragma once

#include "browser/browser_controller.h"
#include "ui/controls/virtual_grid_view.h"

#include <chrono>

namespace files {
  class FileViewAdapter final : public VirtualGridAdapter {
  public:
    FileViewAdapter(
        BrowserController& browser, ViewMode mode, std::function<std::uint32_t()> modifiers,
        std::function<bool()> focused
    );
    [[nodiscard]] std::size_t itemCount() const override;
    [[nodiscard]] std::unique_ptr<Node> createTile() override;
    void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override;
    bool onPointerPress(std::size_t index, float, float, float, float) override;
    bool onPointerDrag(std::optional<std::size_t>, float, float, float, float) override;
    PointerReleaseResult onPointerRelease(std::optional<std::size_t> index) override;
    void onPointerCancel() override;
    void onActivate(std::size_t index) override;

  private:
    BrowserController& m_browser;
    const ViewMode m_mode;
    std::function<std::uint32_t()> m_modifiers;
    std::function<bool()> m_focused;
    std::optional<EntryId> m_pressed;
    std::optional<EntryId> m_lastClick;
    std::chrono::steady_clock::time_point m_lastClickTime;
    bool m_dragged = false;
    bool m_plainClick = false;
  };
} // namespace files
