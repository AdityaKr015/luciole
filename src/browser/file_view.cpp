#include "browser/file_view.h"

#include "browser/file_icon.h"
#include "core/input/key_modifiers.h"
#include "ui/builders.h"

#include <algorithm>
#include <format>
#include <utility>

namespace {
  // Adapted from Noctalia's FileEntryRow, with URI-backed entries and model selection.
  class FileRow final : public InputArea {
  public:
    explicit FileRow(files::FileIconCache& icons) {
      setHitTestVisible(false);
      auto background = ui::box({.out = &m_background, .radius = Style::radiusSm});
      addChild(std::move(background));
      auto icon = std::make_unique<files::FileIcon>(icons, 24.0F);
      m_icon = icon.get();
      auto row = ui::row(
          {.out = &m_row,
           .align = FlexAlign::Center,
           .gap = Style::spaceSm,
           .configure = [](Flex& container) { container.setPadding(0, Style::spaceMd); }},
          std::move(icon),
          ui::column(
              {.align = FlexAlign::Stretch, .flexGrow = 1.0F},
              ui::label(
                  {.out = &m_name,
                   .maxLines = 1,
                   .baselineMode = LabelBaselineMode::TextFixedHeight,
                   .configure = [](Label& label) { label.setUseMarkup(false); }}
              ),
              ui::label(
                  {.out = &m_parent,
                   .fontSize = Style::fontSizeCaption,
                   .maxLines = 1,
                   .configure = [](Label& label) { label.setUseMarkup(false); }}
              )
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
      m_icon->bind(entry.icon, colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurface));
      m_name->setText(entry.displayName);
      m_name->setMaxWidth(std::max(0.0F, width() - 280.0F));
      m_parent->setText(entry.parentDisplayName);
      m_parent->setMaxWidth(std::max(0.0F, width() - 280.0F));
      m_parent->setVisible(!entry.parentDisplayName.empty());
      m_parent->setParticipatesInLayout(!entry.parentDisplayName.empty());
      m_parent->setColor(colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurfaceVariant));
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
    files::FileIcon* m_icon = nullptr;
    Label* m_name = nullptr;
    Label* m_parent = nullptr;
    Label* m_kind = nullptr;
    Label* m_size = nullptr;
  };

  // Adapted from Noctalia's FileEntryTile without its picker or thumbnail service.
  class FileTile final : public InputArea {
  public:
    explicit FileTile(files::FileIconCache& icons) {
      setHitTestVisible(false);
      addChild(ui::box({.out = &m_background, .radius = Style::radiusSm}));
      m_icon = static_cast<files::FileIcon*>(addChild(std::make_unique<files::FileIcon>(icons, 48.0F)));
      addChild(
          ui::label(
              {.out = &m_name,
               .fontSize = Style::fontSizeBody,
               .maxLines = 2,
               .textAlign = TextAlign::Center,
               .baselineMode = LabelBaselineMode::TextFixedHeight,
               .configure = [](Label& label) { label.setUseMarkup(false); }}
          )
      );
      addChild(
          ui::label(
              {.out = &m_parent,
               .fontSize = Style::fontSizeCaption,
               .maxLines = 1,
               .textAlign = TextAlign::Center,
               .configure = [](Label& label) { label.setUseMarkup(false); }}
          )
      );
    }

    void bind(const files::DirectoryEntry& entry, bool selected, bool focused, bool hovered, float iconSize) {
      m_iconSize = iconSize;
      m_icon->setIconSize(iconSize);
      m_icon->bind(entry.icon, colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurface));
      m_name->setText(entry.displayName);
      m_parent->setText(entry.parentDisplayName);
      m_parent->setMaxWidth(std::max(0.0F, width() - Style::spaceSm * 2));
      m_parent->setVisible(!entry.parentDisplayName.empty());
      m_parent->setColor(colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurfaceVariant));
      m_name->setMaxWidth(std::max(0.0F, width() - Style::spaceSm * 2));
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
      const float previewInset = Style::spaceSm;
      const float previewWidth = std::max(0.0F, width() - previewInset * 2);
      const float previewHeight = m_iconSize;
      m_background->arrange(renderer, {0, 0, width(), height()});
      m_icon->arrange(
          renderer, {previewInset + (previewWidth - m_iconSize) * 0.5F, previewInset, m_iconSize, m_iconSize}
      );
      m_name->arrange(
          renderer,
          {Style::spaceSm, previewInset + previewHeight + Style::spaceXs, std::max(0.0F, width() - Style::spaceSm * 2),
           40.0F}
      );
      if (m_parent->visible()) {
        m_parent->arrange(
            renderer, {Style::spaceSm, height() - 24.0F, std::max(0.0F, width() - Style::spaceSm * 2), 20.0F}
        );
      }
    }

  private:
    Box* m_background = nullptr;
    files::FileIcon* m_icon = nullptr;
    Label* m_name = nullptr;
    Label* m_parent = nullptr;
    float m_iconSize = 48.0F;
  };
} // namespace

files::FileViewAdapter::FileViewAdapter(
    BrowserController& browser, FileIconCache& icons, ViewMode mode, std::function<std::uint32_t()> modifiers,
    std::function<bool()> focused
)
    : m_browser(browser), m_icons(icons), m_mode(mode), m_modifiers(std::move(modifiers)),
      m_focused(std::move(focused)) {}

std::size_t files::FileViewAdapter::itemCount() const { return m_browser.model().rows().size(); }
std::unique_ptr<Node> files::FileViewAdapter::createTile() {
  if (m_mode == ViewMode::GRID) {
    return std::make_unique<FileTile>(m_icons);
  }
  return std::make_unique<FileRow>(m_icons);
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
      static_cast<FileTile&>(tile).bind(*entry, selected, focused, hovered, m_gridIconSize);
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
  m_collapseSelectionOnClick = m_plainClick && m_browser.model().selection().selected.contains(*m_pressed);
  const auto mode = (modifiers & KeyMod::Shift) != 0
      ? ((modifiers & KeyMod::Ctrl) != 0 ? SelectionMode::ADD_RANGE : SelectionMode::RANGE)
      : ((modifiers & KeyMod::Ctrl) != 0 ? SelectionMode::TOGGLE : SelectionMode::REPLACE);
  if (!m_collapseSelectionOnClick) {
    (void)m_browser.model().select(*m_pressed, mode);
  }
  return true;
}

bool files::FileViewAdapter::onPointerDrag(std::optional<std::size_t>, float, float, float, float) {
  const bool first = !m_dragged;
  m_dragged = true;
  if (first && m_drag && m_pressed && m_browser.model().entry(*m_pressed) != nullptr) {
    m_drag(m_browser.model().selectedIds());
  }
  return false;
}

VirtualGridAdapter::PointerReleaseResult files::FileViewAdapter::onPointerRelease(std::optional<std::size_t> index) {
  m_areaSelection = false;
  m_areaRows.clear();
  m_areaBaseline.clear();
  const auto pressed = std::exchange(m_pressed, std::nullopt);
  const bool dragged = std::exchange(m_dragged, false);
  if (!pressed
      || !index
      || *index >= itemCount()
      || m_browser.model().rows()[*index] != *pressed
      || dragged
      || !m_plainClick) {
    m_lastClick.reset();
    return {};
  }
  const auto now = std::chrono::steady_clock::now();
  if (m_collapseSelectionOnClick) {
    m_browser.model().select(*pressed);
  }
  const bool twice = m_lastClick == pressed && now - m_lastClickTime < std::chrono::milliseconds(400);
  m_lastClick = twice ? std::nullopt : pressed;
  m_lastClickTime = now;
  return {.rebind = true, .activateIndex = twice ? index : std::nullopt};
}

void files::FileViewAdapter::onPointerCancel() {
  const bool dragged = std::exchange(m_dragged, false);
  m_pressed.reset();
  m_lastClick.reset();
  m_areaSelection = false;
  m_areaRows.clear();
  m_areaBaseline.clear();
  if (dragged && m_cancelDrag) {
    m_cancelDrag();
  }
}

void files::FileViewAdapter::setContextCallback(std::function<void(std::optional<EntryId>, float, float)> callback) {
  m_context = std::move(callback);
}
void files::FileViewAdapter::setDragCallback(
    std::function<void(const std::vector<EntryId>&)> start, std::function<void()> cancel
) {
  m_drag = std::move(start);
  m_cancelDrag = std::move(cancel);
}
void files::FileViewAdapter::onSecondaryActivate(std::size_t index, float x, float y) {
  auto& model = m_browser.model();
  if (index >= model.rows().size()) {
    return;
  }
  const auto id = model.rows()[index];
  if (!model.selection().selected.contains(id)) {
    model.select(id);
  } else {
    model.setFocus(id);
  }
  if (m_context) {
    m_context(id, x, y);
  }
}
void files::FileViewAdapter::onBackgroundSecondaryActivate(float x, float y) {
  if (m_context) {
    m_context(std::nullopt, x, y);
  }
}

bool files::FileViewAdapter::onBackgroundPointerPress() {
  onPointerCancel();
  auto& model = m_browser.model();
  m_areaRows = model.rows();
  m_areaGeneration = model.snapshot().generation;
  const auto modifiers = m_modifiers();
  m_areaToggle = (modifiers & KeyMod::Ctrl) != 0 && (modifiers & KeyMod::Shift) == 0;
  if ((modifiers & (KeyMod::Ctrl | KeyMod::Shift)) != 0) {
    m_areaBaseline = model.selection().selected;
  } else {
    model.clearSelection();
  }
  m_areaSelection = true;
  return true;
}

bool files::FileViewAdapter::onAreaSelection(const std::vector<std::size_t>& indices) {
  auto& model = m_browser.model();
  if (!areaSelectionCurrent()) {
    return false;
  }
  auto selected = m_areaBaseline;
  for (const auto index : indices) {
    if (index >= m_areaRows.size()) {
      return false;
    }
    const auto id = m_areaRows[index];
    if (!m_areaToggle || !selected.erase(id)) {
      selected.insert(id);
    }
  }
  std::vector<EntryId> ids;
  ids.reserve(selected.size());
  for (const auto id : m_areaRows) {
    if (selected.contains(id)) {
      ids.push_back(id);
    }
  }
  return model.replaceSelection(ids);
}

bool files::FileViewAdapter::areaSelectionCurrent() const {
  const auto& model = m_browser.model();
  return m_areaSelection && model.snapshot().generation == m_areaGeneration && model.rows() == m_areaRows;
}

bool files::FileViewAdapter::itemHitTest(std::size_t, float x, float y, float width, float height) const {
  if (m_mode == ViewMode::LIST) {
    return true;
  }
  const float iconLeft = (width - m_gridIconSize) * 0.5F;
  const bool icon = x >= iconLeft - 4 && x <= iconLeft + m_gridIconSize + 4 && y >= 4 && y <= m_gridIconSize + 8;
  const bool label = x >= 8 && x <= width - 8 && y >= m_gridIconSize + 12 && y <= height - 4;
  return icon || label;
}

void files::FileViewAdapter::onActivate(std::size_t index) {
  if (index < itemCount()) {
    m_browser.activate(m_browser.model().rows()[index]);
  }
}
