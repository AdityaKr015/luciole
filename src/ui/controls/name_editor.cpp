#include "ui/controls/name_editor.h"

#include "ui/builders.h"

NameEditor::NameEditor(
    float width, const std::string& title, const std::string& value, std::string hint,
    std::function<std::optional<std::string>(const std::string&)> validate, std::function<void(std::string)> submit,
    std::function<void()> cancel
)
    : m_hint(std::move(hint)), m_validate(std::move(validate)), m_submit(std::move(submit)) {
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
          ui::label({.text = title, .fontWeight = FontWeight::SemiBold, .flexGrow = 1.0F}),
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
           .value = value,
           .placeholder = "Name",
           .onChange = [this](const std::string&) { showError({}); },
           .onSubmit = [this](const std::string&) { this->submit(); }}
      )
  );
  addChild(
      ui::column(
          {.align = FlexAlign::Stretch, .minHeight = 44.0F, .maxHeight = 44.0F},
          ui::label(
              {.out = &m_message,
               .text = m_hint,
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

void NameEditor::setSaving(bool saving) {
  m_saving = saving;
  m_input->setEnabled(!saving);
  m_save->setEnabled(!saving);
  m_save->setText(saving ? "Renaming…" : "Rename");
  m_cancel->setText(saving ? "Close" : "Cancel");
}

void NameEditor::showError(const std::string& error) {
  m_input->setInvalid(!error.empty());
  m_message->setText(error.empty() ? m_hint : error);
  m_message->setColor(colorSpecFromRole(error.empty() ? ColorRole::OnSurfaceVariant : ColorRole::Error));
}

void NameEditor::submit() {
  if (m_saving) {
    return;
  }
  if (const auto error = m_validate(m_input->value())) {
    showError(*error);
    return;
  }
  if (m_submit) {
    m_submit(m_input->value());
  }
}
