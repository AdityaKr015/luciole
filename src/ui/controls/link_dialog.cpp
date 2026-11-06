#include "ui/controls/link_dialog.h"

#include "fs/filename.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/segmented.h"

LinkDialog::LinkDialog(
    float width, std::string source, std::string name, std::string destination, bool hardAllowed,
    std::function<void(std::string, std::string, files::CreateKind)> submit, std::function<void()> cancel
)
    : m_submit(std::move(submit)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceSm);
  setPadding(Style::spaceMd);
  setFill(colorSpecFromRole(ColorRole::SurfaceContainer));
  setRadius(Style::radiusLg);
  setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm},
          ui::label({.text = "Create link", .fontWeight = FontWeight::SemiBold, .flexGrow = 1.0F}),
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
  auto scroll =
      ui::scrollView({.out = &m_scroll, .viewportPaddingH = 0.0F, .viewportPaddingV = 0.0F, .flexGrow = 1.0F});
  scroll->setReserveScrollbarGutter(true);
  scroll->content()->setDirection(FlexDirection::Vertical);
  scroll->content()->setAlign(FlexAlign::Stretch);
  auto body = ui::column(
      {.align = FlexAlign::Stretch, .gap = Style::spaceSm},
      ui::label(
          {.text = std::move(source),
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxWidth = width - 48.0F,
           .maxLines = 2,
           .configure = [](Label& label) { label.setUseMarkup(false); }}
      ),
      ui::label({.text = "Name"}),
      ui::input(
          {.out = &m_name,
           .value = std::move(name),
           .onChange = [this](const std::string&) { showError({}); },
           .onSubmit = [this](const std::string&) { this->submit(); }}
      ),
      ui::label({.text = "Destination folder"}),
      ui::input(
          {.out = &m_destination,
           .value = std::move(destination),
           .onChange = [this](const std::string&) { showError({}); },
           .onSubmit = [this](const std::string&) { this->submit(); }}
      ),
      ui::label({.text = "Link type"})
  );
  if (hardAllowed) {
    body->addChild(
        ui::segmented(
            {.out = &m_kind,
             .options =
                 std::vector<ui::SegmentedOption>{
                     {.label = "Symbolic link", .glyph = {}, .tooltip = {}},
                     {.label = "Hard link", .glyph = {}, .tooltip = {}}
                 },
             .equalSegmentWidths = true,
             .onChange = [this](std::size_t) { showError({}); }}
        )
    );
  } else {
    body->addChild(ui::label({.text = "Symbolic link"}));
  }
  scroll->content()->addChild(std::move(body));
  addChild(std::move(scroll));
  addChild(
      ui::label(
          {.out = &m_error,
           .color = colorSpecFromRole(ColorRole::Error),
           .maxWidth = width - 24.0F,
           .maxLines = 2,
           .visible = false,
           .participatesInLayout = false,
           .configure = [](Label& label) { label.setUseMarkup(false); }}
      )
  );
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm},
          ui::button(
              {.out = &m_cancel, .text = "Cancel", .variant = ButtonVariant::Ghost, .onClick = std::move(cancel)}
          ),
          ui::button({.out = &m_create, .text = "Create", .variant = ButtonVariant::Primary, .onClick = [this] {
                        this->submit();
                      }})
      )
  );
}
void LinkDialog::setSaving(bool saving) {
  m_saving = saving;
  m_name->setEnabled(!saving);
  m_destination->setEnabled(!saving);
  if (m_kind) {
    m_kind->setEnabled(!saving);
  }
  m_create->setEnabled(!saving);
  m_create->setText(saving ? "Creating…" : "Create");
  m_cancel->setText(saving ? "Close" : "Cancel");
}
void LinkDialog::showError(const std::string& error) {
  m_error->setText(error);
  m_error->setVisible(!error.empty());
  m_error->setParticipatesInLayout(!error.empty());
  markLayoutDirty();
}
void LinkDialog::reveal(const Node* node) { m_scroll->requestScrollIntoView(node); }
void LinkDialog::submit() {
  if (m_saving) {
    return;
  }
  if (const auto error = files::filenameProblem(m_name->value())) {
    showError(*error);
  } else if (m_destination->value().empty()) {
    showError("Enter a destination folder.");
  } else {
    m_submit(
        m_name->value(), m_destination->value(),
        m_kind && m_kind->selectedIndex() == 1 ? files::CreateKind::HardLink : files::CreateKind::SymbolicLink
    );
  }
}
