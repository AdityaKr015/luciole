#include "ui/controls/file_preview_view.h"

#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <format>

FilePreviewView::FilePreviewView(
    files::FileIconCache& icons, std::function<void()> close, std::function<void()> open, std::function<void()> openWith
) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setPadding(Style::spaceMd);
  setGap(Style::spaceMd);
  setFill(colorSpecFromRole(ColorRole::SurfaceContainer));
  setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm},
          ui::label(
              {.out = &m_heading, .text = "Preview", .fontWeight = FontWeight::SemiBold, .maxLines = 1, .flexGrow = 1}
          ),
          ui::button(
              {.glyph = "x", .variant = ButtonVariant::Ghost, .tooltip = "Close preview", .onClick = std::move(close)}
          )
      )
  );
  auto body = ui::scrollView({.viewportPaddingH = 0, .viewportPaddingV = 0, .fillWidth = true, .flexGrow = 1});
  m_scroll = body.get();
  body->setReserveScrollbarGutter(true);
  body->content()->setAlign(FlexAlign::Stretch);
  body->content()->setGap(Style::spaceMd);
  auto image = std::make_unique<files::FileIcon>(icons, 192);
  m_image = image.get();
  body->content()->addChild(ui::row({.justify = FlexJustify::Center}, std::move(image)));
  const auto literal = [](Label& label) { label.setUseMarkup(false); };
  body->content()->addChild(
      ui::label({.out = &m_name, .fontWeight = FontWeight::SemiBold, .maxLines = 3, .configure = literal})
  );
  body->content()->addChild(
      ui::label(
          {.out = &m_metadata,
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 8,
           .configure = literal}
      )
  );
  body->content()->addChild(ui::label({.out = &m_status, .maxLines = 3, .configure = literal}));
  body->content()->addChild(ui::label({.out = &m_text, .configure = [](Label& label) {
                                         label.setUseMarkup(false);
                                         label.setFontFamily("monospace");
                                         label.setMaxLines(400);
                                       }}));
  addChild(std::move(body));
  addChild(
      ui::row(
          {.out = &m_actions, .align = FlexAlign::Stretch, .gap = Style::spaceSm},
          ui::button(
              {.out = &m_open,
               .text = "Open",
               .variant = ButtonVariant::Ghost,
               .flexGrow = 1,
               .onClick = std::move(open)}
          ),
          ui::button(
              {.out = &m_openWith,
               .text = "Open With…",
               .variant = ButtonVariant::Ghost,
               .tooltip = "Open With…",
               .flexGrow = 1,
               .onClick = std::move(openWith)}
          )
      )
  );
}
void FilePreviewView::sync(
    const files::PreviewSnapshot& snapshot, float width, float height, std::string_view typeLabel, bool quick
) {
  const float budget = std::max(40.0F, width - Style::spaceMd * 2 - Style::scrollbarWidth - Style::scrollbarGap);
  m_heading->setMaxWidth(std::max(1.0F, width - Style::spaceMd * 2 - Style::controlHeightSm - Style::spaceSm));
  m_actions->setDirection(width < 240 ? FlexDirection::Vertical : FlexDirection::Horizontal);
  for (auto* button : {m_open, m_openWith})
    button->setMaxWidth(std::max(40.0F, width - Style::spaceMd * 2));
  for (auto* label : {m_name, m_metadata, m_status, m_text})
    label->setMaxWidth(budget);
  const auto& entry = snapshot.entry;
  const std::string location = entry ? entry->location.uri() : "";
  if (location != m_location) {
    m_location = location;
    m_scroll->requestScrollToOffset(0);
  }
  m_name->setText(entry ? entry->displayName : "Select an item to preview");
  std::string metadata;
  if (entry) {
    metadata = !typeLabel.empty()              ? std::string(typeLabel)
        : entry->symlink                       ? "Symbolic link"
        : entry->type == G_FILE_TYPE_DIRECTORY ? "Folder"
                                               : entry->contentType.value_or("File");
    if (entry->size && entry->type != G_FILE_TYPE_DIRECTORY) {
      const files::GString size(g_format_size_full(*entry->size, G_FORMAT_SIZE_IEC_UNITS));
      metadata += std::format("\n{}", size.get());
    }
    if (entry->modifiedSeconds && *entry->modifiedSeconds <= G_MAXINT64) {
      const std::unique_ptr<GDateTime, decltype(&g_date_time_unref)> date(
          g_date_time_new_from_unix_local(static_cast<gint64>(*entry->modifiedSeconds)), &g_date_time_unref
      );
      if (date) {
        const files::GString formatted(g_date_time_format(date.get(), "%Y-%m-%d %H:%M"));
        metadata += std::format("\n{}", formatted.get());
      }
    }
    m_image->bind(entry->icon, colorSpecFromRole(ColorRole::OnSurface));
  }
  m_image->setIconSize(quick ? std::min(budget, 460.0F) : budget);
  float imageHeight = 96;
  if (snapshot.image && snapshot.image->pixels) {
    const auto& image = *snapshot.image->pixels;
    imageHeight = (quick ? std::min(budget, 460.0F) : budget)
        * static_cast<float>(image.height)
        / static_cast<float>(image.width);
  }
  m_image->setPreviewHeight(std::clamp(imageHeight, 48.0F, std::max(80.0F, height - 250.0F)));
  m_image->setPreparedImage(snapshot.image);
  const bool showImage = entry && snapshot.image && snapshot.image->pixels;
  m_image->parent()->setVisible(showImage);
  m_image->parent()->setParticipatesInLayout(showImage);
  m_metadata->setText(metadata);
  m_status->setText(snapshot.message);
  m_status->setColor(
      colorSpecFromRole(snapshot.status == files::PreviewStatus::ERROR ? ColorRole::Error : ColorRole::OnSurfaceVariant)
  );
  m_text->setText(snapshot.text);
  m_text->setVisible(!snapshot.text.empty());
  m_text->setParticipatesInLayout(!snapshot.text.empty());
  m_open->setEnabled(entry.has_value());
  m_openWith->setEnabled(entry && entry->type == G_FILE_TYPE_REGULAR && !entry->symlink);
}
void FilePreviewView::clear(Renderer& renderer) { m_image->releaseFile(renderer); }
