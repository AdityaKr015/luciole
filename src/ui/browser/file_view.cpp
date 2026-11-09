#include "ui/browser/file_view.h"

#include "core/input/key_modifiers.h"
#include "core/log.h"
#include "fs/gio_ptr.h"
#include "model/details_columns.h"
#include "ui/browser/file_icon.h"
#include "ui/browser/file_view_metrics.h"
#include "ui/builders.h"
#include "ui/controls/selection_surface.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace {
  constexpr float kHiddenIconOpacity = 0.6F;
  constexpr float kHiddenTextOpacity = 0.7F;

  std::string modifiedTime(std::optional<std::uint64_t> seconds, const char* format) {
    if (!seconds || *seconds > static_cast<std::uint64_t>(G_MAXINT64)) {
      return "Unknown";
    }
    const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> date(
        g_date_time_new_from_unix_local(static_cast<gint64>(*seconds)), &g_date_time_unref
    );
    if (!date) {
      return "Unknown";
    }
    const files::GString text(g_date_time_format(date.get(), format));
    return text ? text.get() : "Unknown";
  }

  std::string fileSize(const files::DirectoryEntry& entry) {
    std::string size;
    if (entry.type != files::FileType::Directory && entry.size) {
      constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
      auto value = static_cast<double>(*entry.size);
      std::size_t unit = 0;
      while (value >= 1024 && unit + 1 < std::size(units)) {
        value /= 1024;
        ++unit;
      }
      size = unit == 0 ? std::format("{} B", *entry.size) : std::format("{:.1f} {}", value, units[unit]);
    } else if (entry.type != files::FileType::Directory) {
      size = "Unknown";
    }
    return size;
  }
  // Adapted from Noctalia's FileEntryRow, with URI-backed entries and model selection.
  class FileRow final : public InputArea {
  public:
    FileRow(files::FileIconCache& icons, files::ThumbnailService& thumbnails, bool miller = false) : m_miller(miller) {
      setHitTestVisible(false);
      addChild(ui::box({.out = &m_background, .radius = SelectionSurface::cornerRadius}));
      auto icon = std::make_unique<files::FileIcon>(icons, 24.0F, &thumbnails);
      m_icon = icon.get();
      auto row = ui::row(
          {.out = &m_row,
           .align = FlexAlign::Center,
           .gap = Style::spaceSm,
           .configure = [](Flex& container) { container.setPadding(0, Style::spaceMd); }},
          std::move(icon),
          ui::column(
              {.out = &m_nameColumn, .align = FlexAlign::Stretch},
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
               .minWidth = 144.0F,
               .maxWidth = 144.0F,
               .maxLines = 1}
          ),
          ui::label(
              {.out = &m_size,
               .fontSize = Style::fontSizeCaption,
               .minWidth = 110.0F,
               .maxWidth = 110.0F,
               .maxLines = 1,
               .textAlign = TextAlign::Start}
          ),
          ui::label(
              {.out = &m_modified,
               .fontSize = Style::fontSizeCaption,
               .minWidth = 148.0F,
               .maxWidth = 148.0F,
               .maxLines = 1}
          )
      );
      row->addChild(ui::glyph({.out = &m_arrow, .glyph = "chevron-right", .glyphSize = 14.0F}));
      addChild(std::move(row));
    }

    void bind(
        const files::DirectoryEntry& entry, std::string_view typeLabel, bool selected, bool focused, bool hovered,
        files::DetailsColumns columns, files::DetailsColumnOrder order, bool cut, bool path,
        files::BrowsePresentation presentation, bool readOnly
    ) {
      m_columns = columns;
      m_columnOrder = order;
      m_icon->setOpacity(cut ? 0.4F : entry.hidden ? kHiddenIconOpacity : 1.0F);
      for (auto* label : {m_name, m_parent, m_kind, m_size, m_modified}) {
        label->setOpacity(entry.hidden && !selected ? kHiddenTextOpacity : 1.0F);
      }
      const bool directory = entry.type == files::FileType::Directory;
      m_arrow->setVisible(m_miller && directory);
      m_arrow->setParticipatesInLayout(m_miller && directory);
      m_arrow->setColor(colorSpecFromRole(path ? ColorRole::Primary : ColorRole::OnSurfaceVariant));
      m_icon->bindFile(entry, colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurface));
      m_iconSize = files::fileRowIconSize(presentation);
      m_icon->setIconSize(m_iconSize);
      m_icon->setEmblems(presentation.showEmblems && entry.symlink, presentation.showEmblems && readOnly);
      m_name->setText(entry.displayName);
      m_parent->setText(entry.parentDisplayName);
      m_fullDate = modifiedTime(entry.modifiedSeconds, "%Y-%m-%d %H:%M");
      m_parent->setVisible(!entry.parentDisplayName.empty());
      m_parent->setParticipatesInLayout(!entry.parentDisplayName.empty());
      m_parent->setColor(colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurfaceVariant));
      m_kind->setUseMarkup(false);
      m_kind->setText(typeLabel);
      m_size->setText(fileSize(entry));
      const auto text = colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurface);
      m_name->setColor(path ? colorSpecFromRole(ColorRole::Primary) : text);
      m_kind->setColor(colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurfaceVariant));
      m_size->setColor(colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurfaceVariant));
      m_modified->setColor(colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurfaceVariant));
      m_background->setFill(
          !selected && (hovered || path) ? colorSpecFromRole(ColorRole::SurfaceContainerHigh) : clearColorSpec()
      );
      if (!selected && focused) {
        m_background->setBorder(colorSpecFromRole(ColorRole::Primary), Style::focusRingWidth);
      } else {
        m_background->clearBorder();
      }
    }

  protected:
    void doLayout(Renderer& renderer) override {
      if (m_columnOrder != m_appliedColumnOrder) {
        for (const auto column : m_columnOrder) {
          Node* label = column == files::DetailsColumn::Type ? m_kind
              : column == files::DetailsColumn::Size         ? m_size
                                                             : m_modified;
          m_row->addChild(m_row->removeChild(label));
        }
        m_appliedColumnOrder = m_columnOrder;
      }
      const auto& columns = m_columns;
      m_kind->setVisible(columns.showType);
      m_kind->setParticipatesInLayout(columns.showType);
      m_size->setVisible(columns.showSize);
      m_size->setParticipatesInLayout(columns.showSize);
      m_modified->setVisible(columns.showModified);
      m_modified->setParticipatesInLayout(columns.showModified);
      m_size->setMinWidth(columns.sizeWidth);
      m_size->setMaxWidth(columns.sizeWidth);
      m_modified->setMinWidth(columns.modifiedWidth);
      m_modified->setMaxWidth(columns.modifiedWidth);
      m_modified->setText(m_fullDate);
      m_kind->setMinWidth(columns.typeWidth);
      m_kind->setMaxWidth(columns.typeWidth);
      m_nameColumn->setMinWidth(columns.nameWidth - m_iconSize - 16.0F);
      m_nameColumn->setMaxWidth(columns.nameWidth - m_iconSize - 16.0F);
      const float nameWidth =
          m_miller ? std::max(32.0F, width() - m_iconSize - 64.0F) : columns.nameWidth - m_iconSize - 16.0F;
      m_nameColumn->setMinWidth(nameWidth);
      m_nameColumn->setMaxWidth(nameWidth);
      m_name->setMaxWidth(nameWidth);
      m_parent->setMaxWidth(nameWidth);
      m_background->arrange(renderer, {0, 0, width(), height()});
      m_row->arrange(renderer, {0, 0, width(), height()});
    }

  public:
    void setThumbnailVisible(bool visible) { m_icon->setThumbnailVisible(visible); }
    void releaseFile(Renderer& renderer) { m_icon->releaseFile(renderer); }

  private:
    Box* m_background = nullptr;
    Flex* m_row = nullptr;
    Flex* m_nameColumn = nullptr;
    files::FileIcon* m_icon = nullptr;
    Label* m_name = nullptr;
    Label* m_parent = nullptr;
    Label* m_kind = nullptr;
    Label* m_size = nullptr;
    Label* m_modified = nullptr;
    Glyph* m_arrow = nullptr;
    bool m_miller;
    float m_iconSize = 24.0F;
    std::string m_fullDate;
    files::DetailsColumns m_columns;
    files::DetailsColumnOrder m_columnOrder = files::kDefaultDetailsColumnOrder;
    files::DetailsColumnOrder m_appliedColumnOrder = files::kDefaultDetailsColumnOrder;
  };

  // Adapted from Noctalia's FileEntryTile without its picker or thumbnail service.
  class FileTile final : public InputArea {
  public:
    FileTile(files::FileIconCache& icons, files::ThumbnailService& thumbnails) {
      setHitTestVisible(false);
      addChild(ui::box({.out = &m_background, .radius = SelectionSurface::cornerRadius}));
      m_icon = static_cast<files::FileIcon*>(addChild(std::make_unique<files::FileIcon>(icons, 48.0F, &thumbnails)));
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
      for (auto& caption : m_captions) {
        addChild(
            ui::label(
                {.out = &caption,
                 .fontSize = Style::fontSizeCaption,
                 .maxLines = 1,
                 .textAlign = TextAlign::Center,
                 .configure = [](Label& label) { label.setUseMarkup(false); }}
            )
        );
      }
    }

    void bind(
        const files::DirectoryEntry& entry, std::string_view typeLabel, bool selected, bool focused, bool hovered,
        float iconSize, bool cut, files::BrowsePresentation presentation, bool readOnly
    ) {
      m_icon->setOpacity(cut ? 0.4F : entry.hidden ? kHiddenIconOpacity : 1.0F);
      for (auto* label : {m_name, m_parent, m_captions[0], m_captions[1], m_captions[2]}) {
        label->setOpacity(entry.hidden && !selected ? kHiddenTextOpacity : 1.0F);
      }
      m_presentation = presentation;
      m_iconSize = iconSize;
      m_icon->setIconSize(iconSize);
      m_icon->bindFile(entry, colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurface));
      m_icon->setEmblems(presentation.showEmblems && entry.symlink, presentation.showEmblems && readOnly);
      const std::array captionText{
          presentation.captions.size ? fileSize(entry) : std::string{},
          presentation.captions.type ? std::string(typeLabel) : std::string{},
          presentation.captions.modified ? modifiedTime(entry.modifiedSeconds, "%Y-%m-%d") : std::string{}
      };
      for (std::size_t i = 0; i < m_captions.size(); ++i) {
        auto* caption = m_captions[i];
        caption->setText(captionText[i]);
        caption->setVisible(!captionText[i].empty());
        caption->setParticipatesInLayout(!captionText[i].empty());
        caption->setColor(colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurfaceVariant));
      }
      m_name->setText(entry.displayName);
      m_parent->setText(entry.parentDisplayName);
      m_parent->setMaxWidth(std::max(0.0F, width() - Style::spaceSm * 2));
      m_parent->setVisible(!entry.parentDisplayName.empty());
      m_parent->setColor(colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurfaceVariant));
      m_name->setMaxWidth(std::max(0.0F, width() - Style::spaceSm * 2));
      m_name->setColor(colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurface));
      m_background->setFill(
          !selected && hovered ? colorSpecFromRole(ColorRole::SurfaceContainerHigh) : clearColorSpec()
      );
      if (!selected && focused) {
        m_background->setBorder(colorSpecFromRole(ColorRole::Primary), Style::focusRingWidth);
      } else {
        m_background->clearBorder();
      }
    }

  protected:
    void doLayout(Renderer& renderer) override {
      const auto metrics = files::gridMetrics(m_iconSize, m_presentation, m_parent->visible());
      const float labelWidth = std::max(1.0F, width() - metrics.inset * 2);
      m_background->arrange(renderer, {0, 0, width(), height()});
      m_icon->arrange(renderer, {(width() - m_iconSize) * 0.5F, metrics.inset, m_iconSize, m_iconSize});
      m_name->setMaxWidth(labelWidth);
      LayoutConstraints nameConstraints;
      nameConstraints.setExactWidth(labelWidth);
      const float nameHeight = std::clamp(m_name->Node::measure(renderer, nameConstraints).height, 1.0F, 40.0F);
      m_name->arrange(renderer, {metrics.inset, metrics.nameTop, labelWidth, nameHeight});
      float y = metrics.nameTop + nameHeight;
      for (auto* caption : m_captions) {
        if (caption->visible()) {
          caption->setMaxWidth(labelWidth);
          caption->arrange(renderer, {metrics.inset, y, labelWidth, 18.0F});
          y += 18.0F;
        }
      }
      if (m_parent->visible()) {
        m_parent->setMaxWidth(labelWidth);
        m_parent->arrange(renderer, {metrics.inset, y, labelWidth, 20.0F});
      }
    }

  public:
    void setThumbnailVisible(bool visible) { m_icon->setThumbnailVisible(visible); }
    void releaseFile(Renderer& renderer) { m_icon->releaseFile(renderer); }

  private:
    Box* m_background = nullptr;
    files::FileIcon* m_icon = nullptr;
    Label* m_name = nullptr;
    Label* m_parent = nullptr;
    float m_iconSize = 48.0F;
    files::BrowsePresentation m_presentation;
    std::array<Label*, 3> m_captions{};
  };
} // namespace

files::FileViewAdapter::FileViewAdapter(
    BrowserController& browser, FileIconCache& icons, ThumbnailService& thumbnails, ViewMode mode,
    std::function<std::uint32_t()> modifiers, std::function<bool()> focused
)
    : m_browser(browser), m_icons(icons), m_thumbnails(thumbnails), m_mode(mode), m_modifiers(std::move(modifiers)),
      m_focused(std::move(focused)) {}

std::size_t files::FileViewAdapter::itemCount() const { return m_browser.model().rows().size(); }
std::unique_ptr<Node> files::FileViewAdapter::createTile() {
  if (m_mode == ViewMode::Grid) {
    return std::make_unique<FileTile>(m_icons, m_thumbnails);
  }
  return std::make_unique<FileRow>(m_icons, m_thumbnails, m_mode == ViewMode::Miller);
}

std::unique_ptr<Node> files::FileViewAdapter::createDecoration() { return std::make_unique<SelectionSurface>(); }
void files::FileViewAdapter::bindDecoration(Node& decoration, const VirtualGridDecorationLayout& layout) {
  const auto& model = m_browser.model();
  std::vector<std::uint8_t> selected(((layout.itemCount + layout.columns - 1) / layout.columns) * layout.columns, 0);
  for (std::size_t i = 0; i < layout.itemCount && layout.firstIndex + i < model.rows().size(); ++i) {
    const auto col = i % layout.columns;
    const auto visual = (i / layout.columns) * layout.columns + (layout.rtl ? layout.columns - col - 1 : col);
    selected[visual] =
        m_columnActive && model.selection().selected.contains(model.rows()[layout.firstIndex + i]) ? 1 : 0;
  }
  decoration.setVisible(std::ranges::any_of(selected, [](auto value) { return value != 0; }));
  static_cast<SelectionSurface&>(decoration)
      .bind(
          std::move(selected), layout.columns, layout.cellWidth + layout.columnGap, layout.cellHeight + layout.rowGap
      );
}

std::string files::FileViewAdapter::itemTooltip(std::size_t index) const {
  const auto& model = m_browser.model();
  if (m_mode == ViewMode::Grid || index >= model.rows().size()) {
    return {};
  }
  const auto id = model.rows()[index];
  const auto* entry = model.entry(id);
  return entry != nullptr ? std::format("{}\n{}", entry->displayName, model.typeLabel(id)) : std::string{};
}

void files::FileViewAdapter::setTileVisible(Node& tile, bool visible) {
  if (m_mode == ViewMode::Grid) {
    static_cast<FileTile&>(tile).setThumbnailVisible(visible);
  } else {
    static_cast<FileRow&>(tile).setThumbnailVisible(visible);
  }
}
void files::FileViewAdapter::releaseTile(Node& tile, Renderer& renderer) {
  if (m_mode == ViewMode::Grid) {
    static_cast<FileTile&>(tile).releaseFile(renderer);
  } else {
    static_cast<FileRow&>(tile).releaseFile(renderer);
  }
}
void files::FileViewAdapter::bindTile(Node& tile, std::size_t index, bool, bool hovered) {
  const auto& model = m_browser.model();
  if (index >= model.rows().size()) {
    return;
  }
  const auto id = model.rows()[index];
  const auto* entry = model.entry(id);
  if (entry != nullptr) {
    const bool selected = m_columnActive && model.selection().selected.contains(id);
    const bool focused = selected || (m_focused() && model.selection().focused == id);
    if (m_mode == ViewMode::Grid) {
      static_cast<FileTile&>(tile).bind(
          *entry, model.typeLabel(id), selected, focused, hovered, m_gridIconSize, m_cut && m_cut(entry->location),
          m_presentation, entry->access.write == false || m_readOnly == true
      );
    } else {
      static_cast<FileRow&>(tile).bind(
          *entry, model.typeLabel(id), selected, focused, hovered, m_columns, m_columnOrder,
          m_cut && m_cut(entry->location), m_pathEntry == id && !m_columnActive, m_presentation,
          entry->access.write == false || m_readOnly == true
      );
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
  m_toggleSelectionOnClick = modifiers == KeyMod::Ctrl && m_browser.model().selection().selected.contains(*m_pressed);
  const auto mode = (modifiers & KeyMod::Shift) != 0
      ? ((modifiers & KeyMod::Ctrl) != 0 ? SelectionMode::AddRange : SelectionMode::Range)
      : ((modifiers & KeyMod::Ctrl) != 0 ? SelectionMode::Toggle : SelectionMode::Replace);
  if (!m_collapseSelectionOnClick && !m_toggleSelectionOnClick) {
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
  if (!pressed || !index || *index >= itemCount() || m_browser.model().rows()[*index] != *pressed || dragged) {
    m_lastClick.reset();
    return {};
  }
  if (m_toggleSelectionOnClick) {
    m_lastClick.reset();
    (void)m_browser.model().select(*pressed, SelectionMode::Toggle);
    return {.rebind = true, .activateIndex = std::nullopt};
  }
  if (!m_plainClick) {
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
  const auto* entry = m_browser.model().entry(*pressed);
  const bool columnFolder = m_mode == ViewMode::Miller && entry && entry->type == files::FileType::Directory;
  return {
      .rebind = true,
      .activateIndex =
          (twice || (!columnFolder && m_activationMode == ActivationMode::SingleClick)) ? index : std::nullopt
  };
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
  if (m_mode != ViewMode::Grid) {
    return true;
  }
  const auto metrics = gridMetrics(m_gridIconSize, m_presentation, false);
  const float iconLeft = (width - m_gridIconSize) * 0.5F;
  const bool icon = x >= iconLeft - 4
      && x <= iconLeft + m_gridIconSize + 4
      && y >= metrics.inset - 4
      && y <= metrics.inset + m_gridIconSize + 4;
  const bool label = x >= metrics.inset && x <= width - metrics.inset && y >= metrics.nameTop && y <= height - 4;
  return icon || label;
}

void files::FileViewAdapter::onActivate(std::size_t index) {
  Logger("file-launch").debug("File-view activation received");
  if (index < itemCount()) {
    if (m_activate) {
      m_activate(m_browser.model().rows()[index]);
    }
  }
}
