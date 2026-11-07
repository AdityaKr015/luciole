#include "ui/controls/permission_editor.h"

#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/controls/recursive_permission_editor.h"
#include "ui/controls/segmented.h"
#include "ui/controls/toggle.h"

#include <algorithm>
#include <format>

namespace {
  constexpr std::array<std::uint32_t, 12> bits{0400, 0200, 0100, 0040,  0020,  0010,
                                               0004, 0002, 0001, 04000, 02000, 01000};
  constexpr float labelWidth = 88.0F, columnWidth = 100.0F;
} // namespace
PermissionEditor::PermissionEditor(std::function<void()> edited, bool multiple, bool folders)
    : m_multiple(multiple), m_edited(std::move(edited)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceSm);
  if (folders) {
    addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm},
            ui::toggle(
                {.out = &m_recursive,
                 .onChange =
                     [this](bool active) {
                       m_direct->setVisible(!active);
                       m_direct->setParticipatesInLayout(!active);
                       m_tree->setVisible(active);
                       m_tree->setParticipatesInLayout(active);
                       changed();
                     }}
            ),
            ui::label({.text = "Include folder contents"})
        )
    );
    auto tree = std::make_unique<RecursivePermissionEditor>([this] { changed(); });
    m_tree = tree.get();
    tree->setVisible(false);
    tree->setParticipatesInLayout(false);
    addChild(std::move(tree));
  }
  auto direct = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm});
  m_direct = direct.get();
  direct->addChild(ui::label({.text = "Permissions", .fontWeight = FontWeight::SemiBold}));
  if (multiple) {
    constexpr std::array names{"Owner read",     "Owner write",   "Owner execute", "Group read",
                               "Group write",    "Group execute", "Others read",   "Others write",
                               "Others execute", "Set user ID",   "Set group ID",  "Sticky bit"};
    for (std::size_t i = 0; i < bits.size(); ++i) {
      direct->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm},
              ui::label({.text = names[i], .minWidth = 104.0F, .maxWidth = 104.0F}),
              ui::segmented(
                  {.out = &m_choices[i],
                   .options =
                       std::vector<ui::SegmentedOption>{
                           {.label = "Keep", .glyph = {}, .tooltip = {}},
                           {.label = "Allow", .glyph = {}, .tooltip = {}},
                           {.label = "Deny", .glyph = {}, .tooltip = {}}
                       },
                   .equalSegmentWidths = true,
                   .flexGrow = 1.0F,
                   .onChange = [this](std::size_t) { changed(); }}
              ),
              ui::label(
                  {.out = &m_current[i], .fontSize = Style::fontSizeCaption, .minWidth = 72.0F, .maxWidth = 72.0F}
              )
          )
      );
    }
  } else {
    auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm});
    header->addChild(ui::label({.text = "", .minWidth = labelWidth, .maxWidth = labelWidth}));
    for (const auto* text : {"Read", "Write", "Execute"}) {
      header->addChild(
          ui::column(
              {.align = FlexAlign::Center, .width = columnWidth},
              ui::label({.text = text, .fontSize = Style::fontSizeCaption})
          )
      );
    }
    direct->addChild(std::move(header));
    constexpr std::array names{"Owner", "Group", "Others"};
    for (std::size_t row = 0; row < names.size(); ++row) {
      auto controls = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm});
      controls->addChild(ui::label({.text = names[row], .minWidth = labelWidth, .maxWidth = labelWidth}));
      for (std::size_t column = 0; column < 3; ++column) {
        controls->addChild(
            ui::column(
                {.align = FlexAlign::Center, .width = columnWidth},
                ui::toggle({.out = &m_bits[row * 3 + column], .onChange = [this](bool) { changed(); }})
            )
        );
      }
      direct->addChild(std::move(controls));
    }
  }
  direct->addChild(ui::label({.out = &m_mode, .fontSize = Style::fontSizeCaption}));
  direct->addChild(ui::label({.text = "Ownership", .fontWeight = FontWeight::SemiBold}));
  const auto change = [this](const std::string&) { changed(); };
  direct->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm},
          ui::label({.text = "Owner", .minWidth = labelWidth, .maxWidth = labelWidth}),
          ui::input({.out = &m_owner, .placeholder = "Name or numeric ID", .flexGrow = 1.0F, .onChange = change})
      )
  );
  direct->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceSm},
          ui::label({.text = "Group", .minWidth = labelWidth, .maxWidth = labelWidth}),
          ui::input({.out = &m_group, .placeholder = "Name or numeric ID", .flexGrow = 1.0F, .onChange = change})
      )
  );
  direct->addChild(ui::label({.out = &m_ids, .fontSize = Style::fontSizeCaption, .maxLines = 2}));
  if (!multiple) {
    direct->addChild(ui::label({.text = "Special permissions", .fontWeight = FontWeight::SemiBold}));
    constexpr std::array special{"Set user ID", "Set group ID", "Sticky bit"};
    for (std::size_t i = 0; i < special.size(); ++i) {
      direct->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm},
              ui::toggle({.out = &m_bits[i + 9], .onChange = [this](bool) { changed(); }}),
              ui::label({.text = special[i]})
          )
      );
    }
  }
  for (auto** out : {&m_reason, &m_message}) {
    direct->addChild(
        ui::label({.out = out, .fontSize = Style::fontSizeCaption, .maxLines = 4, .configure = [](Label& label) {
                     label.setUseMarkup(false);
                   }})
    );
  }
  addChild(std::move(direct));
  sync({});
}
std::uint32_t PermissionEditor::mode() const {
  std::uint32_t value = 0;
  if (m_multiple) {
    return value;
  }
  for (std::size_t i = 0; i < bits.size(); ++i) {
    if (m_bits[i]->checked()) {
      value |= bits[i];
    }
  }
  return value;
}
files::PermissionEdit PermissionEditor::edit() const {
  files::PermissionEdit edit;
  if (!m_snapshot.info) {
    return edit;
  }
  const auto& info = *m_snapshot.info;
  if (m_multiple && info.canMode) {
    edit.mask = 0;
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < bits.size(); ++i) {
      const auto choice = m_choices[i]->selectedIndex();
      if (choice != 0) {
        edit.mask |= bits[i];
        if (choice == 1) {
          value |= bits[i];
        }
      }
    }
    if (edit.mask != 0) {
      edit.mode = value;
    }
  } else if (!m_multiple && info.canMode && mode() != info.mode) {
    edit.mode = mode();
  }
  if (info.canOwner && m_owner->value() != m_initialOwner) {
    edit.owner = m_owner->value().substr(0, 1025);
  }
  if (info.canGroup && m_group->value() != m_initialGroup) {
    edit.group = m_group->value().substr(0, 1025);
  }
  return edit;
}
bool PermissionEditor::canApply() const {
  if (recursive()) {
    return m_tree->canApply();
  }
  const auto value = edit();
  return !(m_snapshot.recursive && m_snapshot.finished)
      && m_snapshot.phase == files::PermissionPhase::Ready
      && (value.mode || value.owner || value.group);
}
void PermissionEditor::changed() {
  if (!m_syncing) {
    if (!m_multiple) {
      m_mode->setText(std::format("Mode: {:04o}", mode()));
    }
    m_edited();
  }
}
void PermissionEditor::sync(const files::PermissionSnapshot& snapshot) {
  m_syncing = true;
  const bool sameValues = snapshot.info
      && m_snapshot.info
      && snapshot.info->mode == m_snapshot.info->mode
      && snapshot.info->owner == m_snapshot.info->owner
      && snapshot.info->group == m_snapshot.info->group;
  const bool keepDraft = sameValues
      && (snapshot.phase == files::PermissionPhase::Saving
          || (snapshot.phase == files::PermissionPhase::Ready && snapshot.error && !snapshot.applied));
  m_snapshot = snapshot;
  const bool ready = snapshot.phase == files::PermissionPhase::Ready
      && snapshot.info.has_value()
      && !(snapshot.recursive && snapshot.finished);
  if (m_tree) {
    m_tree->sync(snapshot);
    m_recursive->setEnabled(ready);
  }
  for (std::size_t i = 0; i < bits.size(); ++i) {
    if (m_multiple) {
      if (!keepDraft) {
        m_choices[i]->setSelectedIndex(0);
      }
      m_choices[i]->setEnabled(ready && snapshot.info->canMode);
      m_current[i]->setText(
          !snapshot.info                             ? "Unknown"
              : snapshot.unknownItems > 0            ? "Unknown"
              : (snapshot.mixedMode & bits[i]) != 0  ? "Mixed"
              : (snapshot.info->mode & bits[i]) != 0 ? "Allowed"
                                                     : "Denied"
      );
      m_current[i]->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
    } else {
      if (!keepDraft) {
        m_bits[i]->setCheckedImmediate(snapshot.info && (snapshot.info->mode & bits[i]) != 0);
      }
      m_bits[i]->setEnabled(ready && snapshot.info->canMode);
    }
  }
  m_owner->setEnabled(ready && snapshot.info->canOwner);
  m_group->setEnabled(ready && snapshot.info->canGroup);
  m_initialOwner = !m_multiple && snapshot.info
      ? (snapshot.info->ownerName.empty() ? std::to_string(snapshot.info->owner) : snapshot.info->ownerName)
      : "";
  m_initialGroup = !m_multiple && snapshot.info
      ? (snapshot.info->groupName.empty() ? std::to_string(snapshot.info->group) : snapshot.info->groupName)
      : "";
  if (!keepDraft) {
    m_owner->setValue(m_initialOwner);
    m_group->setValue(m_initialGroup);
  }
  m_mode->setText(
      m_multiple          ? std::format(
                                "{} items · {} with unknown metadata", snapshot.results ? snapshot.results->size() : 0,
                                snapshot.unknownItems
                            )
          : snapshot.info ? std::format("Mode: {:04o}", mode())
                          : "Mode: Unknown"
  );
  if (m_multiple) {
    m_ids->setText(
        !snapshot.info ? "Owner: Unknown · Group: Unknown"
                       : std::format(
                             "Owner: {} · Group: {}",
                             snapshot.mixedOwner || snapshot.unknownItems > 0 ? "Mixed / unknown"
                                                                              : std::to_string(snapshot.info->owner),
                             snapshot.mixedGroup || snapshot.unknownItems > 0 ? "Mixed / unknown"
                                                                              : std::to_string(snapshot.info->group)
                         )
    );
    m_owner->setPlaceholder("Keep current owner (name or ID to change)");
    m_group->setPlaceholder("Keep current group (name or ID to change)");
  } else {
    m_ids->setText(
        snapshot.info ? std::format("Owner ID: {} · Group ID: {}", snapshot.info->owner, snapshot.info->group) : ""
    );
  }
  std::string reason = snapshot.info ? snapshot.info->reason : "";
  if (snapshot.info && snapshot.info->multipleLinks) {
    reason += " Changes affect all hard links to this file.";
  }
  if (snapshot.info && snapshot.info->canGroup && !snapshot.info->canOwner) {
    reason += " Changing the owner requires administrator permissions.";
  }
  if (snapshot.info && (snapshot.info->mode & 06000) != 0) {
    reason += " Ownership changes can clear set-user/group-ID bits.";
  }
  if (snapshot.info
      && snapshot.info->acl
      && snapshot.info->acl->supported
      && std::ranges::any_of(snapshot.info->acl->access, [](const auto& entry) {
           return entry.tag == files::AclTag::Mask;
         })) {
    reason += " Group mode bits control the ACL mask. Use ACL to see effective rights.";
  }
  m_reason->setText(reason);
  m_reason->setVisible(!reason.empty());
  m_reason->setParticipatesInLayout(!reason.empty());
  const std::string message = snapshot.phase == files::PermissionPhase::Loading ? "Reading permissions…"
      : snapshot.phase == files::PermissionPhase::Saving                        ? "Saving changes…"
                                                                                : snapshot.message;
  m_message->setText(message);
  m_message->setVisible(!m_multiple && !message.empty());
  m_message->setParticipatesInLayout(!m_multiple && !message.empty());
  m_message->setColor(colorSpecFromRole(snapshot.error ? ColorRole::Error : ColorRole::OnSurfaceVariant));
  m_reason->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_ids->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_syncing = false;
}
bool PermissionEditor::recursive() const { return m_recursive && m_recursive->checked(); }
files::RecursivePermissionEdit PermissionEditor::recursiveEdit() const { return m_tree->edit(); }
const Node* PermissionEditor::messageNode() const {
  return recursive() ? m_tree->messageNode() : m_multiple ? static_cast<const Node*>(m_group) : m_message;
}
