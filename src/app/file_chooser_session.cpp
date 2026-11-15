#include "app/file_chooser_session.h"

#include "app/page/browser_page.h"
#include "app/workspace_layout.h"
#include "core/log.h"
#include "platform/desktop_integration.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <ranges>

namespace {
  const Logger kLog("file-chooser");

  // Listings report links as links, without their target's kind; the chooser policy resolves the selected ones, so
  // every filter keeps links (and entries of unread kind) rather than hiding a linked folder or file.
  bool unresolved(const luciole::EntryBrief& brief) {
    return brief.symlink || brief.type == luciole::FileType::Symlink || brief.type == luciole::FileType::Unknown;
  }

  luciole::DirectoryModel::EntryFilter entryFilter(bool directory, const std::optional<luciole::FileFilter>& filter) {
    using luciole::FileType;
    if (directory) {
      return [](const luciole::EntryBrief& brief, const luciole::DirectoryEntry*) {
        return brief.type == FileType::Directory || unresolved(brief);
      };
    }
    if (!filter) {
      return {};
    }
    return [filter = *filter](const luciole::EntryBrief& brief, const luciole::DirectoryEntry* known) {
      if (brief.type == FileType::Directory || unresolved(brief)) {
        return true;
      }
      const char* type = known != nullptr && known->contentType ? known->contentType->c_str() : nullptr;
      return filter.matches(brief.name, type);
    };
  }
} // namespace

FileChooserSession::FileChooserSession(
    luciole::FileChooserRequest request, std::shared_ptr<luciole::FileChooserCall> call, WaylandConnection& connection,
    RenderContext& renderer
)
    : m_request(std::move(request)), m_call(std::move(call)), m_connection(connection),
      m_dropdown(connection, renderer) {
  m_filters = m_request.filters;
  if (m_request.currentFilter) {
    const auto found = std::ranges::find(m_filters, *m_request.currentFilter);
    if (found == m_filters.end()) {
      m_filters.push_back(*m_request.currentFilter);
      m_activeFilter = m_filters.size() - 1;
    } else {
      m_activeFilter = static_cast<std::size_t>(found - m_filters.begin());
    }
  }
  if (!m_filters.empty()) {
    m_filter.emplace(m_filters[m_activeFilter].patterns);
  }
  for (const auto& choice : m_request.choices) {
    if (choice.options.empty()) {
      m_choices.emplace_back(choice.id, choice.initial == "true" ? "true" : "false");
    } else {
      const auto found = std::ranges::find(choice.options, choice.initial, &std::pair<std::string, std::string>::first);
      m_choices.emplace_back(choice.id, found != choice.options.end() ? found->first : choice.options.front().first);
    }
  }
}

FileChooserSession::~FileChooserSession() { m_dropdown.closeSelectDropdown(); }

std::string FileChooserSession::windowTitle() const {
  if (!m_request.title.empty()) {
    return m_request.title;
  }
  return m_request.directory ? "Open Folder" : m_request.multiple ? "Open Files" : "Open File";
}

luciole::ChooserMode FileChooserSession::mode() const {
  return {.directory = m_request.directory, .multiple = m_request.multiple};
}

luciole::ChooserDecision FileChooserSession::decision(const std::vector<luciole::DirectoryEntry>& entries) const {
  return luciole::chooserDecision(mode(), m_page->location(), entries);
}

std::unique_ptr<Node>
FileChooserSession::attach(BrowserPage& page, luciole::WindowSurface& surface, std::function<void()> close) {
  m_page = &page;
  m_surface = &surface;
  m_close = std::move(close);
  m_call->setCloseCallback(m_close);
  page.setChooser(
      {.open =
           [this](const std::vector<luciole::DirectoryEntry>& targets) {
             // Activating one linked folder enters it, as activating a folder does, in either mode.
             if (targets.size() == 1) {
               auto entered = luciole::chooserDecision({}, m_page->location(), targets);
               if (entered.kind == luciole::ChooserDecision::Kind::Enter) {
                 apply(entered);
                 return;
               }
             }
             // Enter or a double click on one item of a selection opens the whole selection.
             auto selection = luciole::chooserSelection(m_page->browser().model());
             const bool inSelection = targets.size() == 1
                 && std::ranges::any_of(selection, [&](const auto& entry) { return entry.id == targets.front().id; });
             apply(decision(inSelection ? selection : targets));
           },
       .cancel = [this] { cancel(); }}
  );
  page.browser().model().setSingleSelection(!m_request.multiple);
  if (auto filter = entryFilter(m_request.directory, m_filter)) {
    page.browser().model().setEntryFilter(std::move(filter));
  }

  auto footer = ui::row(
      {.align = FlexAlign::Center,
       .gap = Style::spaceSm,
       .paddingH = Style::spaceMd,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
       .height = kChooserFooterHeight}
  );
  const auto plain = [](Label& label) { label.setUseMarkup(false); };
  for (std::size_t index = 0; index < m_request.choices.size(); ++index) {
    const auto& choice = m_request.choices[index];
    if (choice.options.empty()) {
      footer->addChild(
          ui::checkbox({.checked = m_choices[index].second == "true", .onChange = [this, index](bool checked) {
                          m_choices[index].second = checked ? "true" : "false";
                        }})
      );
      footer->addChild(ui::label({.text = choice.label, .configure = plain}));
      continue;
    }
    std::vector<std::string> labels;
    labels.reserve(choice.options.size());
    std::size_t selected = 0;
    for (std::size_t option = 0; option < choice.options.size(); ++option) {
      labels.push_back(choice.options[option].second);
      if (choice.options[option].first == m_choices[index].second) {
        selected = option;
      }
    }
    footer->addChild(ui::label({.text = choice.label, .configure = plain}));
    footer->addChild(
        ui::select(
            {.options = std::move(labels),
             .selectedIndex = selected,
             .onSelectionChanged = [this, index](std::size_t option, std::string_view) {
               m_choices[index].second = m_request.choices[index].options.at(option).first;
             }}
        )
    );
  }
  if (!m_filters.empty()) {
    std::vector<std::string> names;
    names.reserve(m_filters.size());
    for (const auto& filter : m_filters) {
      names.push_back(filter.name);
    }
    footer->addChild(
        ui::select(
            {.options = std::move(names),
             .selectedIndex = m_activeFilter,
             .onSelectionChanged = [this](std::size_t index, std::string_view) { selectFilter(index); }}
        )
    );
  }
  footer->addChild(
      ui::label(
          {.out = &m_reason,
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 1,
           .textAlign = TextAlign::End,
           .ellipsize = TextEllipsize::End,
           .flexGrow = 1.0F,
           .configure = plain}
      )
  );
  footer->addChild(ui::button({.text = "Cancel", .variant = ButtonVariant::Ghost, .minWidth = 80.0F, .onClick = [this] {
                                 cancel();
                               }}));
  const std::string action = !m_request.acceptLabel.empty() ? m_request.acceptLabel
      : m_request.directory                                 ? "Select"
                                                            : "Open";
  footer->addChild(
      ui::button(
          {.out = &m_accept,
           .text = action,
           .enabled = false,
           .variant = ButtonVariant::Primary,
           .minWidth = 88.0F,
           .onClick = [this] { apply(decision(luciole::chooserSelection(m_page->browser().model()))); }}
      )
  );
  footer->setPopupContext(&m_dropdown);
  return footer;
}

void FileChooserSession::sync() {
  if (m_page == nullptr) {
    return;
  }
  // The dropdown opens on the window's output, which is known only once the window entered one.
  if (auto* output = m_connection.outputForSurface(m_surface->wlSurface()); output && output != m_dropdownOutput) {
    m_dropdownOutput = output;
    m_dropdown.setParent(m_surface->xdgSurface(), m_surface->wlSurface(), output);
  }
  const auto current = decision(luciole::chooserSelection(m_page->browser().model()));
  m_accept->setEnabled(current.kind != luciole::ChooserDecision::Kind::Unavailable);
  m_reason->setText(current.reason);
}

void FileChooserSession::selectFilter(std::size_t index) {
  if (index >= m_filters.size()) {
    return;
  }
  m_activeFilter = index;
  m_filter.emplace(m_filters[index].patterns);
  m_page->browser().model().setEntryFilter(entryFilter(m_request.directory, m_filter));
}

void FileChooserSession::apply(const luciole::ChooserDecision& decision) {
  if (m_call->finished()) {
    return;
  }
  switch (decision.kind) {
  case luciole::ChooserDecision::Kind::Unavailable:
    return;
  case luciole::ChooserDecision::Kind::Enter:
    m_page->open(decision.locations.front());
    return;
  case luciole::ChooserDecision::Kind::Accept: {
    luciole::FileChooserResult result;
    for (const auto& location : decision.locations) {
      result.uris.push_back(location.uri());
    }
    if (!m_filters.empty()) {
      result.currentFilter = m_filters[m_activeFilter];
    }
    result.choices = m_choices;
    kLog.debug("accepting {} item(s)", result.uris.size());
    m_call->complete(result);
    m_close();
    return;
  }
  }
}

void FileChooserSession::cancel() {
  m_call->cancel();
  m_close();
}

bool FileChooserSession::pointer(const PointerEvent& event) {
  if (!m_dropdown.isSelectDropdownOpen()) {
    return false;
  }
  if (event.surface == m_dropdown.wlSurface()) {
    (void)m_dropdown.onPointerEvent(event);
    return true;
  }
  if (event.type == PointerEvent::Type::Button && event.pressed) {
    // Like a native select: a press outside only closes the dropdown.
    m_dropdown.closeSelectDropdown();
    return true;
  }
  return false;
}

bool FileChooserSession::keyboard(const KeyboardEvent& event) {
  if (!m_dropdown.isSelectDropdownOpen()) {
    return false;
  }
  m_dropdown.onKeyboardEvent(event);
  return true;
}

void FileChooserSession::windowClosed() {
  m_dropdown.closeSelectDropdown();
  m_call->cancel();
}
