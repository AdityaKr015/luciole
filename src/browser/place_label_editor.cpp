#include "browser/place_label_editor.h"

#include "config/state_store.h"
#include "ui/builders.h"

files::PlaceLabelEditor::PlaceLabelEditor(
    float width, const std::string& label, std::function<void(std::string)> submit, std::function<void()> cancel
)
    : m_submit(std::move(submit)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceMd);
  setPadding(Style::spaceMd);
  setFill(colorSpecFromRole(ColorRole::SurfaceContainer));
  setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
  setRadius(Style::radiusLg);
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm},
          ui::label({.text = "Rename place", .fontWeight = FontWeight::SemiBold, .flexGrow = 1.0F}),
          ui::button(
              {.glyph = "x",
               .controlHeight = Style::controlHeightSm,
               .variant = ButtonVariant::Default,
               .tooltip = "Close",
               .minWidth = Style::controlHeightSm,
               .maxWidth = Style::controlHeightSm,
               .onClick = cancel}
          )
      )
  );
  addChild(
      ui::input(
          {.out = &m_input,
           .value = label,
           .placeholder = "Places label",
           .onChange = [this](const std::string&) { showError({}); },
           .onSubmit = [this](const std::string&) { this->submit(); }}
      )
  );
  addChild(
      ui::column(
          {.align = FlexAlign::Stretch, .minHeight = 44.0F, .maxHeight = 44.0F},
          ui::label(
              {.out = &m_message,
               .text = "Changes the sidebar label only.",
               .fontSize = Style::fontSizeCaption,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxWidth = width - 24.0F,
               .maxLines = 3,
               .configure = [](Label& message) { message.setUseMarkup(false); }}
          )
      )
  );
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
          ui::button(
              {.out = &m_cancel,
               .text = "Cancel",
               .variant = ButtonVariant::Ghost,
               .minWidth = 80.0F,
               .onClick = std::move(cancel)}
          ),
          ui::button(
              {.out = &m_save,
               .text = "Rename",
               .variant = ButtonVariant::Primary,
               .minWidth = 88.0F,
               .onClick = [this] { this->submit(); }}
          )
      )
  );
}

void files::PlaceLabelEditor::setSaving(bool saving) {
  m_saving = saving;
  m_input->setEnabled(!saving);
  m_save->setEnabled(!saving);
  m_save->setText(saving ? "Saving…" : "Rename");
  m_cancel->setText(saving ? "Close" : "Cancel");
}

void files::PlaceLabelEditor::showError(const std::string& error) {
  m_input->setInvalid(!error.empty());
  m_message->setText(error.empty() ? "Changes the sidebar label only." : error);
  m_message->setColor(colorSpecFromRole(error.empty() ? ColorRole::OnSurfaceVariant : ColorRole::Error));
}

void files::PlaceLabelEditor::submit() {
  if (m_saving) {
    return;
  }
  if (const auto error = bookmarkLabelProblem(m_input->value())) {
    showError(*error);
    return;
  }
  if (m_input->value().find_first_not_of(' ') == std::string::npos) {
    showError("Enter a label.");
    return;
  }
  if (m_submit) {
    m_submit(m_input->value());
  }
}
