#include "ui/controls/tooltip_presenter.h"

#include "render/scene/input_area.h"
#include "ui/builders.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace {
  constexpr auto kShowDelay = std::chrono::milliseconds(500);
  constexpr float kMaxContentWidth = 280.0F;
  constexpr int kMaxTextLines = 3;
  constexpr float kPadH = Style::spaceMd + Style::borderWidth;
  constexpr float kPadV = Style::spaceSm + Style::borderWidth;
} // namespace

TooltipPresenter::TooltipPresenter(std::function<void()> requestUpdate) : m_requestUpdate(std::move(requestUpdate)) {
  // Keep Noctalia's tooltip typography, padding, surface and outline. Ordinary
  // application windows use a noninteractive overlay instead of shell popups.
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setPadding(kPadV, kPadH);
  setFill(colorSpecFromRole(ColorRole::Surface));
  setRadius(Style::scaledRadiusMd());
  setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
  setParticipatesInLayout(false);
  setHitTestVisible(false);
  setExcludeSubtreeFromTabOrder(true);
  setZIndex(100);
  setVisible(false);
  addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeCaption,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxWidth = kMaxContentWidth,
          .maxLines = kMaxTextLines,
          .configure = [](Label& label) { label.setUseMarkup(false); },
      })
  );
}

void TooltipPresenter::dismiss() {
  m_delay.stop();
  m_identity = nullptr;
  m_text.clear();
  m_ready = false;
  m_armed = false;
  setVisible(false);
}

void TooltipPresenter::sync(Renderer& renderer, Node& root, float pointerX, float pointerY, LayoutSize bounds) {
  if (!m_armed)
    return;
  InputArea* area = nullptr;
  for (auto* node = Node::hitTest(&root, pointerX, pointerY); node; node = node->parent()) {
    if (auto* input = dynamic_cast<InputArea*>(node); input && input->hasTooltip()) {
      area = input;
      break;
    }
  }
  const auto content = area ? area->tooltipContent() : TooltipContent{};
  const auto* text = std::get_if<std::string>(&content);
  if (!text || text->empty()) {
    dismiss();
    return;
  }
  const auto* anchorNode = area->tooltipAnchorNode() ? area->tooltipAnchorNode() : area;
  LayoutRect anchor{0, 0, anchorNode->width(), anchorNode->height()};
  Node::absolutePosition(anchorNode, anchor.x, anchor.y);
  if (area->hasTooltipAnchorInsets()) {
    const auto inset = area->tooltipAnchorInsets();
    anchor.x += inset.left;
    anchor.y += inset.top;
    anchor.width = std::max(1.0F, anchor.width - inset.left - inset.right);
    anchor.height = std::max(1.0F, anchor.height - inset.top - inset.bottom);
  }
  const auto placement = area->tooltipPlacement();
  if (area != m_identity
      || *text != m_text
      || placement != m_placement
      || anchor.x != m_anchor.x
      || anchor.y != m_anchor.y
      || anchor.width != m_anchor.width
      || anchor.height != m_anchor.height) {
    m_identity = area;
    m_text = *text;
    m_anchor = anchor;
    m_placement = placement;
    m_ready = false;
    setVisible(false);
    m_delay.start(kShowDelay, [this] {
      m_ready = true;
      m_requestUpdate();
    });
  }
  if (!m_ready)
    return;

  m_label->setText(m_text);
  m_label->setMaxWidth(std::max(1.0F, std::min(kMaxContentWidth, bounds.width - 2 * (Style::spaceSm + kPadH))));
  const auto size = measure(renderer, LayoutConstraints::available(bounds.width, bounds.height));
  float x = anchor.x + (anchor.width - size.width) * 0.5F;
  float y = anchor.y + anchor.height + Style::spaceXs;
  if (placement == TooltipPlacement::Above
      || ((placement == TooltipPlacement::Default || placement == TooltipPlacement::Below)
          && y + size.height > bounds.height - Style::spaceSm))
    y = anchor.y - size.height - Style::spaceSm;
  else if (placement == TooltipPlacement::Left || placement == TooltipPlacement::Right) {
    x = placement == TooltipPlacement::Left ? anchor.x - size.width - Style::spaceSm
                                            : anchor.x + anchor.width + Style::spaceSm;
    y = anchor.y + (anchor.height - size.height) * 0.5F;
  }
  x = std::clamp(x, Style::spaceSm, std::max(Style::spaceSm, bounds.width - size.width - Style::spaceSm));
  y = std::clamp(y, Style::spaceSm, std::max(Style::spaceSm, bounds.height - size.height - Style::spaceSm));
  setVisible(true);
  arrange(renderer, {x, y, size.width, size.height});
}
