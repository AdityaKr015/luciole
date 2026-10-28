#include "browser/file_view.h"

#include "core/input/key_modifiers.h"
#include "ui/builders.h"

#include <algorithm>
#include <format>
#include <utility>

namespace {
  // Adapted from Noctalia's FileEntryRow, with URI-backed entries and model selection.
  class FileRow final : public InputArea {
  public:
    FileRow() {
      setHitTestVisible(false);
      auto background = ui::box({.out = &m_background, .radius = Style::radiusSm});
      addChild(std::move(background));
      auto row = ui::row(
          {.out = &m_row,
           .align = FlexAlign::Center,
           .gap = Style::spaceSm,
           .configure = [](Flex& container) { container.setPadding(0, Style::spaceMd); }},
          ui::glyph({.out = &m_icon, .glyphSize = Style::fontSizeBody}),
          ui::label(
              {.out = &m_name,
               .maxLines = 1,
               .baselineMode = LabelBaselineMode::TextFixedHeight,
               .flexGrow = 1.0F,
               .configure = [](Label& label) { label.setUseMarkup(false); }}
          ),
          ui::label(
              {.out = &m_kind,
               .fontSize = Style::fontSizeCaption,
               .minWidth = 100.0F,
               .maxWidth = 100.0F,
               .maxLines = 1}
          ),
          ui::label(
              {.out = &m_size,
               .fontSize = Style::fontSizeCaption,
               .minWidth = 110.0F,
               .maxWidth = 110.0F,
               .maxLines = 1,
               .textAlign = TextAlign::End}
          )
      );
      addChild(std::move(row));
    }

    void bind(const files::DirectoryEntry& entry, bool selected, bool focused, bool hovered) {
      const bool directory = entry.type == G_FILE_TYPE_DIRECTORY;
      m_icon->setGlyph(entry.symlink ? "link" : directory ? "folder" : "file");
      m_name->setText(entry.displayName);
      m_name->setMaxWidth(std::max(0.0F, width() - 280.0F));
      m_kind->setText(
          entry.symlink                           ? "Link"
              : directory                         ? "Folder"
              : entry.type == G_FILE_TYPE_REGULAR ? "File"
              : entry.type == G_FILE_TYPE_SPECIAL ? "Special"
                                                  : "Unknown"
      );
      std::string size;
      if (!directory && entry.size) {
        constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
        auto value = static_cast<double>(*entry.size);
        std::size_t unit = 0;
        while (value >= 1024 && unit + 1 < std::size(units)) {
          value /= 1024;
          ++unit;
        }
        size = unit == 0 ? std::format("{} B", *entry.size) : std::format("{:.1f} {}", value, units[unit]);
      } else if (!directory) {
        size = "Unknown";
      }
      m_size->setText(size);
      const auto text = colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurface);
      m_name->setColor(text);
      m_icon->setColor(text);
      m_kind->setColor(colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurfaceVariant));
      m_size->setColor(colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurfaceVariant));
      m_background->setFill(colorSpecFromRole(
          selected      ? ColorRole::PrimaryContainer
              : hovered ? ColorRole::SurfaceContainerHigh
                        : ColorRole::Surface
      ));
      if (focused) {
        m_background->setBorder(colorSpecFromRole(ColorRole::Primary), Style::focusRingWidth);
      } else {
        m_background->clearBorder();
      }
    }

  protected:
    void doLayout(Renderer& renderer) override {
      m_background->arrange(renderer, {0, 0, width(), height()});
      m_row->arrange(renderer, {0, 0, width(), height()});
    }

  private:
    Box* m_background = nullptr;
    Flex* m_row = nullptr;
    Glyph* m_icon = nullptr;
    Label* m_name = nullptr;
    Label* m_kind = nullptr;
    Label* m_size = nullptr;
  };

  // Adapted from Noctalia's FileEntryTile without its picker or thumbnail service.
  class FileTile final : public InputArea {
  public:
    FileTile() {
      setHitTestVisible(false);
      addChild(ui::box({.out = &m_background, .radius = Style::radiusLg}));
      addChild(
          ui::box(
              {.out = &m_preview, .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow), .radius = Style::radiusMd}
          )
      );
      addChild(ui::glyph({.out = &m_icon, .glyphSize = 36.0F}));
      addChild(
          ui::label(
              {.out = &m_name,
               .fontSize = Style::fontSizeCaption,
               .maxLines = 2,
               .textAlign = TextAlign::Center,
               .baselineMode = LabelBaselineMode::TextFixedHeight,
               .configure = [](Label& label) { label.setUseMarkup(false); }}
          )
      );
    }

    void bind(const files::DirectoryEntry& entry, bool selected, bool focused, bool hovered) {
      m_icon->setGlyph(entry.symlink ? "link" : entry.type == G_FILE_TYPE_DIRECTORY ? "folder" : "file");
      m_name->setText(entry.displayName);
      m_name->setMaxWidth(std::max(0.0F, width() - Style::spaceSm * 2));
      m_icon->setColor(colorSpecFromRole(ColorRole::OnSurface));
      m_name->setColor(colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurface));
      m_background->setFill(colorSpecFromRole(
          selected      ? ColorRole::PrimaryContainer
              : hovered ? ColorRole::SurfaceContainerHigh
                        : ColorRole::Surface
      ));
      if (focused) {
        m_background->setBorder(colorSpecFromRole(ColorRole::Primary), Style::focusRingWidth);
      } else {
        m_background->clearBorder();
      }
    }

  protected:
    void doLayout(Renderer& renderer) override {
      const float previewInset = Style::spaceMd;
      const float previewWidth = std::max(0.0F, width() - previewInset * 2);
      const float previewHeight = std::max(0.0F, height() - 60.0F);
      m_background->arrange(renderer, {0, 0, width(), height()});
      m_preview->arrange(renderer, {previewInset, previewInset, previewWidth, previewHeight});
      m_icon->measure(renderer);
      m_icon->arrange(
          renderer,
          {previewInset + (previewWidth - m_icon->width()) * 0.5F,
           previewInset + (previewHeight - m_icon->height()) * 0.5F, m_icon->width(), m_icon->height()}
      );
      m_name->arrange(
          renderer,
          {Style::spaceSm, previewInset + previewHeight + Style::spaceXs, std::max(0.0F, width() - Style::spaceSm * 2),
           40.0F}
      );
    }

  private:
    Box* m_background = nullptr;
    Box* m_preview = nullptr;
    Glyph* m_icon = nullptr;
    Label* m_name = nullptr;
  };
} // namespace

files::FileViewAdapter::FileViewAdapter(
    BrowserController& browser, ViewMode mode, std::function<std::uint32_t()> modifiers, std::function<bool()> focused
)
    : m_browser(browser), m_mode(mode), m_modifiers(std::move(modifiers)), m_focused(std::move(focused)) {}

std::size_t files::FileViewAdapter::itemCount() const { return m_browser.model().rows().size(); }
std::unique_ptr<Node> files::FileViewAdapter::createTile() {
  if (m_mode == ViewMode::GRID) {
    return std::make_unique<FileTile>();
  }
  return std::make_unique<FileRow>();
}

void files::FileViewAdapter::bindTile(Node& tile, std::size_t index, bool, bool hovered) {
  const auto& model = m_browser.model();
  if (index >= model.rows().size()) {
    return;
  }
  const auto id = model.rows()[index];
  const auto* entry = model.entry(id);
  if (entry != nullptr) {
    const bool selected = model.selection().selected.contains(id);
    const bool focused = m_focused() && model.selection().focused == id;
    if (m_mode == ViewMode::GRID) {
      static_cast<FileTile&>(tile).bind(*entry, selected, focused, hovered);
    } else {
      static_cast<FileRow&>(tile).bind(*entry, selected, focused, hovered);
    }
  }
}

bool files::FileViewAdapter::onPointerPress(std::size_t index, float, float, float, float) {
  if (index >= itemCount()) {
    return true;
  }
  const auto modifiers = m_modifiers();
  m_pressed = m_browser.model().rows()[index];
  m_dragged = false;
  m_plainClick = modifiers == 0;
  const auto mode = (modifiers & KeyMod::Shift) != 0
      ? ((modifiers & KeyMod::Ctrl) != 0 ? SelectionMode::ADD_RANGE : SelectionMode::RANGE)
      : ((modifiers & KeyMod::Ctrl) != 0 ? SelectionMode::TOGGLE : SelectionMode::REPLACE);
  (void)m_browser.model().select(*m_pressed, mode);
  return true;
}

bool files::FileViewAdapter::onPointerDrag(std::optional<std::size_t>, float, float, float, float) {
  m_dragged = true;
  return false;
}

VirtualGridAdapter::PointerReleaseResult files::FileViewAdapter::onPointerRelease(std::optional<std::size_t> index) {
  const auto pressed = std::exchange(m_pressed, std::nullopt);
  if (!pressed
      || !index
      || *index >= itemCount()
      || m_browser.model().rows()[*index] != *pressed
      || m_dragged
      || !m_plainClick) {
    m_lastClick.reset();
    return {};
  }
  const auto now = std::chrono::steady_clock::now();
  const bool twice = m_lastClick == pressed && now - m_lastClickTime < std::chrono::milliseconds(400);
  m_lastClick = twice ? std::nullopt : pressed;
  m_lastClickTime = now;
  return {.rebind = true, .activateIndex = twice ? index : std::nullopt};
}

void files::FileViewAdapter::onPointerCancel() {
  m_pressed.reset();
  m_lastClick.reset();
}

void files::FileViewAdapter::onActivate(std::size_t index) {
  if (index < itemCount()) {
    m_browser.activate(m_browser.model().rows()[index]);
  }
}
