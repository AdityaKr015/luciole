#include "app/page/about_controller.h"

#include "app/page/active_dialog.h"
#include "app/page/page_context.h"
#include "noctalia_files_build_info.h"
#include "platform/file_launcher.h"
#include "ui/builders.h"
#include "ui/controls/scroll_view.h"
#include "ui/dialogs/dialog_chrome.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"

#include <format>

AboutController::AboutController(PageContext& page, files::FileLauncher& launcher)
    : m_page(page), m_launcher(launcher) {}

void AboutController::open() {
  ScrollView* scroll = nullptr;
  Label* message = nullptr;
  constexpr float width = 536.0F;
  auto body = ui::scrollView(
      {.out = &scroll,
       .viewportPaddingH = 0.0F,
       .viewportPaddingV = 0.0F,
       .fillWidth = true,
       .flexGrow = 1.0F,
       .configure = [](ScrollView& view) { view.setReserveScrollbarGutter(true); }}
  );
  body->content()->setAlign(FlexAlign::Stretch);
  body->content()->setGap(Style::spaceMd);
  const auto text = [&](const std::string& value, bool caption = false) {
    body->content()->addChild(
        ui::label(
            {.text = value,
             .fontSize = caption ? Style::fontSizeCaption : Style::fontSizeBody,
             .maxWidth = width,
             .maxLines = 1000,
             .configure = [](Label& label) { label.setUseMarkup(false); }}
        )
    );
  };
  text(
      std::format(
          "Version {} · {}\nRevision {}\nBuilt with {}", NOCTALIA_FILES_VERSION, NOCTALIA_FILES_BUILD_TYPE,
          NOCTALIA_FILES_BUILD_REVISION, NOCTALIA_FILES_COMPILER
      )
  );
  text("A standalone file manager from Noctalia.");
  const auto link = [this](const char* address) { m_launcher.open(files::Location::fromUri(address)); };
  body->content()->addChild(
      ui::row(
          {.gap = Style::spaceSm},
          ui::button(
              {.text = "Project website",
               .variant = ButtonVariant::Default,
               .onClick = [link] { link("https://noctalia.dev"); }}
          ),
          ui::button({.text = "Source code", .variant = ButtonVariant::Default, .onClick = [link] {
                        link("https://github.com/noctalia-dev/noctalia-files");
                      }})
      )
  );
  body->content()->addChild(
      ui::label(
          {.out = &message,
           .color = colorSpecFromRole(ColorRole::Error),
           .maxWidth = width,
           .maxLines = 3,
           .visible = false,
           .participatesInLayout = false,
           .configure = [](Label& label) { label.setUseMarkup(false); }}
      )
  );
  auto root = ui::column(
      {.align = FlexAlign::Stretch},
      files::dialogHeader("About Noctalia Files", "info-circle", [this] { m_page.dismissMenu(); }), std::move(body)
  );
  files::applyDialogStyle(*root);
  m_page.showCenteredDialog(std::move(root), 588.0F);
  if (m_page.menuSurface()) {
    m_page.activeDialog().bind(ActiveDialog::Kind::About, [this] {
      m_scroll = nullptr;
      m_message = nullptr;
    });
    m_scroll = scroll;
    m_message = message;
  }
}

void AboutController::launcherChanged(const std::string& error) {
  if (m_message) {
    m_message->setText(error.empty() ? "" : std::format("Could not open the link: {}", error));
    m_message->setVisible(!error.empty());
    m_message->setParticipatesInLayout(!error.empty());
    m_page.menuSurface()->requestLayout();
  }
}

void AboutController::revealFocus(InputArea* focused) {
  if (m_scroll) {
    m_scroll->requestScrollIntoView(focused);
  }
}
