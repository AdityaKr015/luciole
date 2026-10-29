#include "browser/places_sidebar.h"

#include "browser/location_presentation.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "ui/builders.h"

#include <algorithm>
#include <cmath>

files::PlacesSidebar::PlacesSidebar(PinService& pins, FileIconCache& icons) : m_pins(pins), m_icons(icons) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setMinWidth(206.0F);
  setMaxWidth(206.0F);
  setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
  addChild(
      ui::scrollView(
          {.out = &m_scroll, .viewportPaddingH = 8.0F, .viewportPaddingV = 12.0F, .fillWidth = true, .flexGrow = 1.0F}
      )
  );
  m_scroll->content()->setAlign(FlexAlign::Stretch);
  m_scroll->content()->setGap(Style::spaceXs);
}
void files::PlacesSidebar::setOpenCallback(std::function<void(const Location&)> callback) {
  m_open = std::move(callback);
}
void files::PlacesSidebar::setContextCallback(std::function<void(const Bookmark&, float, float)> callback) {
  m_context = std::move(callback);
}
void files::PlacesSidebar::setDragCallback(std::function<void(const Bookmark&)> callback) {
  m_drag = std::move(callback);
}
void files::PlacesSidebar::setRenameCallback(std::function<void(const Bookmark&)> callback) {
  m_rename = std::move(callback);
}
InputArea* files::PlacesSidebar::focusArea(const Location& location) const {
  const auto found = std::ranges::find_if(m_rows, [&](const auto& row) { return row.bookmark.location == location; });
  return found == m_rows.end() ? nullptr : found->button->inputArea();
}
void files::PlacesSidebar::invalidate() {
  m_dirty = true;
  markLayoutDirty();
}
void files::PlacesSidebar::cancelDragCandidate() {
  m_dragCandidate.reset();
  m_dragged = false;
}
void files::PlacesSidebar::sync(std::optional<Location> current) {
  m_current = std::move(current);
  bool highlighted = false;
  for (auto& row : m_rows) {
    const bool selected = !highlighted && m_current == row.bookmark.location;
    row.button->setSelected(selected);
    row.icon->bind(
        {.names = {row.iconName, "folder"}, .filePath = {}, .desktopFile = {}},
        colorSpecFromRole(selected ? ColorRole::OnPrimary : ColorRole::OnSurface)
    );
    highlighted = highlighted || selected;
  }
  markLayoutDirty();
}
void files::PlacesSidebar::addRow(const Bookmark& bookmark, const std::string& iconName, std::size_t index) {
  auto button = ui::button(
      {.text = bookmark.label,
       .contentAlign = ButtonContentAlign::Start,
       .variant = ButtonVariant::Ghost,
       .paddingH = 8.0F,
       .gap = 8.0F,
       .onClick = [this, location = bookmark.location] {
         if (m_open) {
           m_open(location);
         }
       }}
  );
  auto* raw = button.get();
  raw->setDirection(FlexDirection::Horizontal);
  raw->label()->setUseMarkup(false);
  raw->label()->setMaxWidth(126.0F);
  raw->setTooltip(locationEditText(bookmark.location));
  raw->inputArea()->setTabFocusKey("place:" + bookmark.location.uri());
  raw->inputArea()->setOnKeyDown([this, bookmark, raw](const InputArea::KeyData& key) {
    if (!key.pressed) {
      return;
    }
    if (key.modifiers == 0 && KeySymbol::isEnterOrSpace(key.sym)) {
      if (m_open) {
        m_open(bookmark.location);
      }
    } else if (key.sym == XKB_KEY_F2 && key.modifiers == 0) {
      if (m_rename) {
        m_rename(bookmark);
      }
    } else if (key.sym == XKB_KEY_Menu || (key.sym == XKB_KEY_F10 && key.modifiers == KeyMod::Shift)) {
      float x = 0, y = 0;
      Node::absolutePosition(raw, x, y);
      if (m_context) {
        m_context(bookmark, x + raw->width() * 0.5F, y + raw->height() * 0.5F);
      }
    }
  });
  auto icon = std::make_unique<FileIcon>(m_icons, 24.0F);
  auto* artwork = icon.get();
  artwork->bind(
      {.names = {iconName, "folder"}, .filePath = {}, .desktopFile = {}}, colorSpecFromRole(ColorRole::OnSurface)
  );
  raw->insertChildAt(0, std::move(icon));
  raw->setOnRightClickWithPointer([this, bookmark](float x, float y, std::uint32_t, std::uint32_t) {
    cancelDragCandidate();
    if (m_context) {
      m_context(bookmark, x, y);
    }
  });
  {
    raw->inputArea()->setOnPress([this, bookmark, raw](const InputArea::PointerData& pointer) {
      raw->setPressedVisual(pointer.pressed);
      if (pointer.button == BTN_LEFT && pointer.pressed) {
        m_dragCandidate = bookmark;
        m_pressX = pointer.sceneX;
        m_pressY = pointer.sceneY;
        m_dragged = false;
      } else if (!pointer.pressed) {
        cancelDragCandidate();
      }
    });
    raw->setOnPointerMotion([this, raw](float x, float y) {
      float originX = 0, originY = 0;
      Node::absolutePosition(raw, originX, originY);
      if (m_dragCandidate
          && !m_dragged
          && std::hypot(originX + x - m_pressX, originY + y - m_pressY) >= Style::dragStartThreshold) {
        m_dragged = true;
        const auto bookmark = *m_dragCandidate;
        if (m_drag) {
          m_drag(bookmark);
        }
      }
    });
    raw->inputArea()->setOnCancel([this, raw] {
      raw->setPressedVisual(false);
      cancelDragCandidate();
    });
  }
  raw->inputArea()->setOnFocusGain([this, raw] {
    raw->setKeyboardFocusHint(true);
    m_scroll->requestScrollIntoView(raw);
  });
  raw->inputArea()->setOnFocusLoss([raw] { raw->setKeyboardFocusHint(false); });
  m_rows.push_back({bookmark, iconName, index, raw, artwork});
  m_scroll->content()->addChild(std::move(button));
}
void files::PlacesSidebar::rebuild() {
  cancelDragCandidate();
  m_rows.clear();
  auto* content = m_scroll->content();
  while (!content->children().empty()) {
    content->removeChild(content->children().back().get());
  }
  const auto heading = [&](const std::string& text, Label** out = nullptr) {
    content->addChild(
        ui::label(
            {.out = out,
             .text = text,
             .fontSize = Style::fontSizeCaption,
             .fontWeight = FontWeight::SemiBold,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
             .height = 24.0F}
        )
    );
  };
  heading("Places", &m_heading);
  const auto& pins = m_pins.bookmarks();
  for (std::size_t i = 0; i < pins.size(); ++i) {
    const auto known = std::ranges::find_if(m_pins.userPlaces(), [&](const auto& place) {
      return place.bookmark.location == pins[i].location;
    });
    addRow(pins[i], known == m_pins.userPlaces().end() ? "folder" : known->iconName, i);
  }
  if (pins.empty()) {
    content->addChild(
        ui::label(
            {.text = "Drag folders here to pin",
             .fontSize = Style::fontSizeCaption,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
             .maxWidth = 174.0F,
             .maxLines = 2}
        )
    );
  }
  sync(m_current);
}
void files::PlacesSidebar::doLayout(Renderer& renderer) {
  if (m_dirty) {
    m_dirty = false;
    rebuild();
  }
  Flex::doLayout(renderer);
}
LayoutSize files::PlacesSidebar::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}
void files::PlacesSidebar::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }
std::optional<std::size_t> files::PlacesSidebar::dropSlot(float x, float y) const {
  float localX = 0, localY = 0;
  if (!Node::mapFromScene(this, x, y, localX, localY)
      || localX < 0
      || localY < 0
      || localX >= width()
      || localY >= height()) {
    return std::nullopt;
  }
  for (const auto& row : m_rows) {
    float rowX = 0, rowY = 0;
    Node::absolutePosition(row.button, rowX, rowY);
    if (y < rowY + row.button->height() * 0.5F) {
      return row.pinIndex;
    }
  }
  return m_pins.bookmarks().size();
}
LayoutRect files::PlacesSidebar::dropLine(std::size_t slot) const {
  float x = 0, y = 0;
  Node::absolutePosition(this, x, y);
  const float top = y;
  float line = top + height() - 8.0F;
  if (m_heading != nullptr) {
    float headingX = 0;
    Node::absolutePosition(m_heading, headingX, line);
    line += m_heading->height() + 2.0F;
  }
  for (const auto& row : m_rows) {
    float rowX = 0, rowY = 0;
    Node::absolutePosition(row.button, rowX, rowY);
    line = rowY - 2.0F;
    if (row.pinIndex >= slot) {
      break;
    }
    line = rowY + row.button->height() + 2.0F;
  }
  return {x + 8.0F, std::clamp(line, top + 2.0F, top + height() - 3.0F), width() - 16.0F, 2.0F};
}
void files::PlacesSidebar::dragScroll(float delta) {
  m_scroll->requestScrollToOffset(m_scroll->scrollOffset() + delta);
}
float files::PlacesSidebar::dragScrollSpeed(float x, float y) const {
  float localX = 0, localY = 0;
  if (!Node::mapFromScene(this, x, y, localX, localY) || localX < 0 || localX >= width()) {
    return 0;
  }
  if (localY < 28 && m_scroll->scrollOffset() > 0) {
    return -6;
  }
  if (localY > height() - 28 && m_scroll->scrollOffset() < m_scroll->maxScrollOffset()) {
    return 6;
  }
  return 0;
}
