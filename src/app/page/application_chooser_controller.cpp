#include "app/page/application_chooser_controller.h"

#include "app/page/active_dialog.h"
#include "app/page/page_context.h"
#include "platform/file_launcher.h"
#include "ui/browser/file_icon.h"
#include "ui/builders.h"
#include "ui/controls/dialog_chrome.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"

#include <utility>
#include <vector>

ApplicationChooserController::ApplicationChooserController(
    PageContext& page, files::FileLauncher& launcher, files::FileIconCache& icons
)
    : m_page(page), m_launcher(launcher), m_icons(icons) {}

void ApplicationChooserController::show(const files::DirectoryEntry& entry) {
  Flex* choices = nullptr;
  ScrollView* choiceScroll = nullptr;
  auto scroll = ui::scrollView(
      {.out = &choiceScroll, .viewportPaddingH = 0, .viewportPaddingV = 0, .fillWidth = true, .flexGrow = 1}
  );
  scroll->content()->setAlign(FlexAlign::Stretch);
  scroll->content()->addChild(
      ui::column(
          {.out = &choices, .align = FlexAlign::Stretch, .gap = Style::spaceXs},
          ui::label({.text = "Loading applications…", .maxLines = 2})
      )
  );
  auto root = ui::column(
      {.align = FlexAlign::Stretch}, files::dialogHeader("Open With", "apps", [this] { m_page.dismissMenu(); }),
      std::move(scroll)
  );
  files::applyDialogStyle(*root);
  m_page.showCenteredDialog(std::move(root), 440.0F);
  if (!m_page.menuSurface()) {
    return;
  }
  m_page.activeDialog().bind(ActiveDialog::Kind::Applications, [this] {
    m_container = nullptr;
    m_scroll = nullptr;
  });
  m_container = choices;
  m_scroll = choiceScroll;
  const auto generation = m_page.activeDialog().generation();
  const auto request = m_page.requestFor({entry});
  const std::weak_ptr<int> lifetime = m_lifetime;
  m_launcher.applications(
      entry.contentType.value_or("application/octet-stream"),
      [this, lifetime, generation, request, entry](std::vector<files::ApplicationChoice> apps) {
        if (lifetime.expired()
            || !m_container
            || generation != m_page.activeDialog().generation()
            || !m_page.active()) {
          return;
        }
        while (!m_container->children().empty()) {
          (void)m_container->removeChild(m_container->children().back().get());
        }
        if (!m_page.actionState(files::BrowserAction::OpenWith, request).enabled) {
          apps.clear();
        }
        if (apps.empty()) {
          m_container->addChild(ui::label({.text = "No applications available for this file type.", .maxLines = 3}));
        }
        for (const auto& app : apps) {
          auto button = ui::button(
              {.text = app.name,
               .controlHeight = Style::controlHeight,
               .contentAlign = ButtonContentAlign::Start,
               .variant = ButtonVariant::Ghost,
               .tooltip = app.name,
               .gap = Style::spaceSm,
               .onClick =
                   [this, request, entry, app] {
                     if (m_page.actionState(files::BrowserAction::OpenWith, request).enabled) {
                       m_launcher.openWith(entry.location, app);
                     } else {
                       m_page.actionError("The item changed. Select it again.");
                     }
                     m_page.dismissMenu();
                   },
               .configure = [](Button& button) { button.label()->setUseMarkup(false); }}
          );
          button->setFillWidth(true);
          auto source = app.icon;
          source.names.emplace_back("application-x-executable");
          auto icon = std::make_unique<files::FileIcon>(m_icons, 24.0F);
          icon->bind(source, colorSpecFromRole(ColorRole::OnSurface));
          button->insertChildAt(0, std::move(icon));
          m_container->addChild(std::move(button));
        }
        m_page.menuSurface()->requestLayout();
      }
  );
}

void ApplicationChooserController::revealFocus(InputArea* focused) {
  if (m_scroll) {
    m_scroll->requestScrollIntoView(focused);
  }
}
