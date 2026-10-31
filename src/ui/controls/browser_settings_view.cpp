#include "ui/controls/browser_settings_view.h"

#include "ui/builders.h"
#include "ui/controls/select.h"
#include "ui/controls/stepper.h"

#include <algorithm>
#include <format>

namespace {
  using P = files::BrowserPreference;
  std::vector<int> widths(int minimum, int maximum, bool automatic = false) {
    std::vector<int> result;
    if (automatic)
      result.push_back(0);
    for (int value = minimum; value <= maximum; value += 16)
      result.push_back(value);
    return result;
  }
} // namespace

BrowserSettingsView::BrowserSettingsView(
    std::function<void(files::OverrideEdit)> change, std::function<void()> reset, std::function<void()> close,
    std::function<void()> clearThumbnails
)
    : m_change(std::move(change)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setFill(colorSpecFromRole(ColorRole::Surface));
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .padding = Style::spaceMd},
          ui::label(
              {.text = "Settings",
               .fontSize = Style::fontSizeHeader,
               .fontWeight = FontWeight::SemiBold,
               .flexGrow = 1.0F}
          ),
          ui::button(
              {.glyph = "x",
               .variant = ButtonVariant::Default,
               .minWidth = Style::controlHeight,
               .maxWidth = Style::controlHeight,
               .onClick = std::move(close)}
          )
      )
  );
  auto navigation = ui::column(
      {.out = &m_navigation,
       .align = FlexAlign::Stretch,
       .gap = 4.0F,
       .padding = 8.0F,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
       .width = 152.0F}
  );
  auto body = ui::scrollView(
      {.out = &m_body,
       .viewportPaddingH = 0.0F,
       .viewportPaddingV = 0.0F,
       .fillWidth = true,
       .flexGrow = 1.0F,
       .configure = [](ScrollView& view) { view.setReserveScrollbarGutter(true); }}
  );
  m_body->content()->setAlign(FlexAlign::Stretch);
  m_body->content()->setPadding(16.0F);
  addChild(ui::row({.align = FlexAlign::Stretch, .flexGrow = 1.0F}, std::move(navigation), std::move(body)));
  section("Browsing");
  addChoice(
      P::VIEW_MODE, "View", [](const auto& s) { return s.viewMode; },
      {{"Grid", files::ViewMode::GRID}, {"List", files::ViewMode::LIST}}
  );
  addNumber(
      P::GRID_ICON_SIZE, "Icon size", [](const auto& s) { return s.gridIconSize; },
      {files::kGridIconSizes.begin(), files::kGridIconSizes.end()}
  );
  addChoice(
      P::SORT_BY, "Sort by", [](const auto& s) { return s.sort.field; },
      {{"Name", files::SortField::NAME},
       {"Type", files::SortField::TYPE},
       {"Size", files::SortField::SIZE},
       {"Modified", files::SortField::MODIFIED}}
  );
  addBool(P::SORT_DESCENDING, "Reverse order", [](const auto& s) { return s.sort.descending; });
  addBool(P::DIRECTORIES_FIRST, "Folders first", [](const auto& s) { return s.sort.directoriesFirst; });
  addBool(P::SHOW_HIDDEN, "Show hidden files", [](const auto& s) { return s.showHidden; });
  section("Behavior");
  addChoice(
      P::ACTIVATION_MODE, "Open items", [](const auto& s) { return s.activationMode; },
      {{"Single click", files::ActivationMode::SINGLE_CLICK}, {"Double click", files::ActivationMode::DOUBLE_CLICK}}
  );
  addChoice(
      P::TYPING_MODE, "When typing", [](const auto& s) { return s.typingMode; },
      {{"Search subfolders", files::TypingMode::SEARCH}, {"Select names", files::TypingMode::TYPEAHEAD}}
  );
  section("Sidebar");
  addBool(P::SHOW_SIDEBAR, "Show sidebar", [](const auto& s) { return s.showSidebar; });
  addNumber(
      P::SIDEBAR_WIDTH, "Sidebar width", [](const auto& s) { return s.sidebarWidth; },
      widths(files::kMinSidebarWidth, files::kMaxSidebarWidth)
  );
  section("Preview");
  addBool(P::SHOW_PREVIEW, "Show preview", [](const auto& s) { return s.showPreview; });
  addNumber(P::PREVIEW_WIDTH, "Preview width", [](const auto& s) { return s.previewWidth; }, widths(208, 800));
  section("List columns");
  addBool(P::SHOW_TYPE_COLUMN, "Type", [](const auto& s) { return s.columns.type; });
  addBool(P::SHOW_SIZE_COLUMN, "Size", [](const auto& s) { return s.columns.size; });
  addBool(P::SHOW_MODIFIED_COLUMN, "Modified", [](const auto& s) { return s.columns.modified; });
  addNumber(
      P::NAME_COLUMN_WIDTH, "Name width (0 = auto)", [](const auto& s) { return s.columnWidths[0]; },
      widths(160, 16384, true)
  );
  addNumber(P::TYPE_COLUMN_WIDTH, "Type width", [](const auto& s) { return s.columnWidths[1]; }, widths(64, 2048));
  addNumber(P::SIZE_COLUMN_WIDTH, "Size width", [](const auto& s) { return s.columnWidths[2]; }, widths(64, 2048));
  addNumber(
      P::MODIFIED_COLUMN_WIDTH, "Modified width", [](const auto& s) { return s.columnWidths[3]; }, widths(64, 2048)
  );
  std::vector<Choice> orders;
  auto order = files::kDefaultDetailsColumnOrder;
  do {
    std::string label;
    for (const auto column : order) {
      if (!label.empty())
        label += " / ";
      label += column == files::DetailsColumn::TYPE ? "Type"
          : column == files::DetailsColumn::SIZE    ? "Size"
                                                    : "Modified";
    }
    orders.emplace_back(label, order);
  } while (std::ranges::next_permutation(order).found);
  addChoice(P::COLUMN_ORDER, "Column order", [](const auto& value) { return value.columnOrder; }, orders);
  section("Thumbnails");
  addChoice(
      P::THUMBNAIL_POLICY, "Show thumbnails", [](const auto& value) { return value.thumbnails.policy; },
      {{"Never", files::ThumbnailPolicy::NEVER},
       {"Local files", files::ThumbnailPolicy::LOCAL},
       {"All locations", files::ThumbnailPolicy::ALL}}
  );
  m_section->addChild(
      ui::label(
          {.text = "Remote locations use existing cached thumbnails.",
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 2}
      )
  );
  addNumber(
      P::THUMBNAIL_MAX_FILE_MIB, "Source file limit", [](const auto& value) { return value.thumbnails.maxFileMiB; },
      {1, 1024}, "MiB"
  );
  addNumber(
      P::THUMBNAIL_MAX_MEGAPIXELS, "Image pixel limit",
      [](const auto& value) { return value.thumbnails.maxMegapixels; }, {1, 64}, "MP"
  );
  addNumber(
      P::THUMBNAIL_CACHE_MIB, "Disk cache limit", [](const auto& value) { return value.thumbnails.cacheMiB; },
      {16, 1024}, "MiB"
  );
  m_section->addChild(
      ui::button(
          {.out = &m_clearThumbnails,
           .text = "Clear thumbnail cache",
           .variant = ButtonVariant::Default,
           .onClick = std::move(clearThumbnails)}
      )
  );
  selectPage(0);
  addChild(
      ui::column(
          {.align = FlexAlign::Stretch, .paddingH = Style::spaceMd},
          ui::label(
              {.out = &m_status, .fontSize = Style::fontSizeCaption, .maxLines = 3, .configure = [](Label& label) {
                 label.setUseMarkup(false);
               }}
          )
      )
  );
  addChild(
      ui::row(
          {.justify = FlexJustify::End, .gap = Style::spaceSm, .padding = Style::spaceMd},
          ui::button({.text = "Reset all settings", .variant = ButtonVariant::Default, .onClick = std::move(reset)})
      )
  );
}

void BrowserSettingsView::section(const std::string& title) {
  const auto index = m_pages.size();
  const std::string glyph = index == 0 ? "layout-grid"
      : index == 1                     ? "hand-click"
      : index == 2                     ? "layout-sidebar"
      : index == 3                     ? "columns-3"
                                       : "photo";
  Button* button = nullptr;
  m_navigation->addChild(
      ui::button(
          {.out = &button,
           .text = title,
           .glyph = glyph,
           .fontSize = Style::fontSizeCaption,
           .contentAlign = ButtonContentAlign::Start,
           .variant = ButtonVariant::Tab,
           .paddingH = 8.0F,
           .gap = 6.0F,
           .onClick = [this, index] { selectPage(index); }}
      )
  );
  m_navButtons.push_back(button);
  auto page = ui::column(
      {.align = FlexAlign::Stretch, .gap = 16.0F},
      ui::row(
          {.align = FlexAlign::Center, .gap = 8.0F},
          ui::glyph({.glyph = glyph, .color = colorSpecFromRole(ColorRole::Primary)}),
          ui::label(
              {.text = title,
               .fontSize = Style::fontSizeHeader,
               .fontWeight = FontWeight::SemiBold,
               .color = colorSpecFromRole(ColorRole::Primary)}
          )
      )
  );
  page->addChild(
      ui::column(
          {.out = &m_section,
           .align = FlexAlign::Stretch,
           .gap = 8.0F,
           .padding = 12.0F,
           .fill = colorSpecFromRole(ColorRole::SurfaceContainer, 0.4F),
           .radius = Style::radiusLg}
      )
  );
  m_pages.push_back(page.get());
  m_body->content()->addChild(std::move(page));
}
void BrowserSettingsView::selectPage(std::size_t selected) {
  for (std::size_t index = 0; index < m_pages.size(); ++index) {
    m_pages[index]->setVisible(index == selected);
    m_pages[index]->setParticipatesInLayout(index == selected);
    m_navButtons[index]->setVariant(index == selected ? ButtonVariant::TabActive : ButtonVariant::Tab);
  }
  m_body->requestScrollToOffset(0);
}
void BrowserSettingsView::setCompact(bool compact) {
  const float width = compact ? 152.0F : 192.0F;
  m_navigation->setMinWidth(width);
  m_navigation->setMaxWidth(width);
}
void BrowserSettingsView::addRow(P key, const std::string& title, Read read, std::unique_ptr<Node> control) {
  m_reads[key] = std::move(read);
  Button* reset = nullptr;
  m_section->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = 8.0F, .paddingV = 6.0F},
          ui::label({.text = title, .fontWeight = FontWeight::SemiBold, .maxLines = 2, .flexGrow = 1.0F}),
          ui::button(
              {.out = &reset,
               .text = "Reset",
               .fontSize = Style::fontSizeCaption,
               .variant = ButtonVariant::Ghost,
               .minWidth = 48.0F,
               .maxWidth = 48.0F,
               .paddingH = 4.0F,
               .onClick = [this, key] { send(key, std::monostate{}); }}
          ),
          std::move(control)
      )
  );
  m_resets[key] = reset;
}

void BrowserSettingsView::addChoice(P key, const std::string& title, Read read, const std::vector<Choice>& choices) {
  auto control = std::make_unique<Select>();
  auto* select = control.get();
  std::vector<std::string> options;
  options.reserve(choices.size());
  for (const auto& choice : choices)
    options.push_back(choice.first);
  select->setOptions(std::move(options));
  select->setMinWidth(180.0F);
  select->setMaxWidth(180.0F);
  select->setFontSize(Style::fontSizeCaption);
  select->setOnSelectionChanged([this, key, choices](std::size_t index, std::string_view) {
    send(key, choices[index].second);
  });
  m_apply[key] = [select, choices](const Value& value) {
    const auto found = std::ranges::find(choices, value, &Choice::second);
    select->setSelectedIndexSilently(static_cast<std::size_t>(found - choices.begin()));
  };
  addRow(key, title, std::move(read), std::move(control));
}

void BrowserSettingsView::addBool(P key, const std::string& title, Read read) {
  Toggle* control = nullptr;
  auto row = ui::row(
      {.justify = FlexJustify::End, .width = 180.0F},
      ui::toggle({.out = &control, .onChange = [this, key](bool value) { send(key, value); }})
  );
  m_apply[key] = [control](const Value& value) { control->setCheckedImmediate(std::get<bool>(value)); };
  addRow(key, title, std::move(read), std::move(row));
}

void BrowserSettingsView::addNumber(
    P key, const std::string& title, Read read, const std::vector<int>& values, std::string suffix
) {
  auto control = std::make_unique<Stepper>();
  auto* stepper = control.get();
  stepper->setMinWidth(180.0F);
  stepper->setMaxWidth(180.0F);
  stepper->setRange(values.front(), values.back());
  stepper->setStep(16);
  stepper->setValueSuffix(std::move(suffix));
  stepper->setOnValueChanged([this, key, stepper, values](int value) {
    const int previous = std::get<int>(m_values.at(key));
    if (key == P::GRID_ICON_SIZE && !std::ranges::contains(values, value)) {
      const auto next =
          value > previous ? std::ranges::upper_bound(values, value) : std::ranges::lower_bound(values, value);
      stepper->setValue(
          value > previous ? (next == values.end() ? values.back() : *next)
                           : (next == values.begin() ? values.front() : *std::prev(next))
      );
    } else if (key == P::NAME_COLUMN_WIDTH && value > 0 && value < 160)
      stepper->setValue(value > previous ? 160 : 0);
  });
  stepper->setOnValueCommitted([this, key](int value) { send(key, value); });
  m_apply[key] = [stepper](const Value& value) {
    if (stepper->value() != std::get<int>(value) || !stepper->valueField()->inputArea()->focused())
      stepper->setValue(std::get<int>(value));
  };
  addRow(key, title, std::move(read), std::move(control));
}

void BrowserSettingsView::send(P key, Value value) {
  m_values[key] = std::holds_alternative<std::monostate>(value) ? m_reads.at(key)(m_inherited) : value;
  m_apply.at(key)(m_values.at(key));
  const bool overridden = m_values.at(key) != m_reads.at(key)(m_inherited);
  m_resets.at(key)->setVisible(overridden);
  m_resets.at(key)->setParticipatesInLayout(overridden);
  m_status->setText("Saving…");
  m_status->setVisible(true);
  m_status->setParticipatesInLayout(true);
  m_status->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_change({key, value});
}
void BrowserSettingsView::sync(
    const files::BrowserSettings& settings, const files::BrowserSettings& inherited, bool busy,
    const std::string& error, bool clearingThumbnails
) {
  m_clearThumbnails->setEnabled(!clearingThumbnails);
  m_clearThumbnails->setText(clearingThumbnails ? "Clearing…" : "Clear thumbnail cache");
  m_inherited = inherited;
  for (const auto& [key, read] : m_reads) {
    m_values[key] = read(settings);
    m_apply.at(key)(m_values.at(key));
    const bool overridden = m_values.at(key) != m_reads.at(key)(m_inherited);
    m_resets.at(key)->setVisible(overridden);
    m_resets.at(key)->setParticipatesInLayout(overridden);
  }
  m_status->setText(!error.empty() ? error : clearingThumbnails ? "Clearing cache…" : busy ? "Saving…" : "");
  m_status->setVisible(!error.empty() || busy);
  m_status->setParticipatesInLayout(!error.empty() || busy);
  m_status->setColor(colorSpecFromRole(error.empty() ? ColorRole::OnSurfaceVariant : ColorRole::Error));
}
void BrowserSettingsView::reveal(const Node* node) { m_body->requestScrollIntoView(node); }
