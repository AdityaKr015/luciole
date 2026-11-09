#include "ui/controls/dialog_chrome.h"

#include "ui/builders.h"

namespace files {

  void applyDialogStyle(Flex& root) {
    root.setFill(colorSpecFromRole(ColorRole::Surface));
    root.setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
    root.setRadius(Style::radiusLg);
    root.setPadding(Style::spaceMd);
    root.setGap(Style::spaceMd);
  }

  std::unique_ptr<Flex> dialogHeader(const std::string& title, const std::string& glyph, std::function<void()> close) {
    return ui::column(
        {.align = FlexAlign::Stretch, .gap = Style::spaceSm},
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm},
            ui::glyph({.glyph = glyph, .color = colorSpecFromRole(ColorRole::Primary)}),
            ui::label(
                {.text = title,
                 .fontSize = Style::fontSizeTitle,
                 .fontWeight = FontWeight::SemiBold,
                 .maxLines = 1,
                 .flexGrow = 1.0F,
                 .configure = [](Label& label) { label.setUseMarkup(false); }}
            ),
            ui::button(
                {.glyph = "x",
                 .controlHeight = Style::controlHeightSm,
                 .variant = ButtonVariant::Ghost,
                 .tooltip = "Close",
                 .minWidth = Style::controlHeightSm,
                 .maxWidth = Style::controlHeightSm,
                 .onClick = std::move(close)}
            )
        ),
        ui::separator({})
    );
  }

} // namespace files
