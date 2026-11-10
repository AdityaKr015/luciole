#include "ui/controls/acl_editor.h"

#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/permission_editor.h"
#include "ui/controls/toggle.h"

#include <algorithm>
#include <array>
#include <format>

namespace {
  using Tag = luciole::AclTag;
  bool named(Tag tag) { return tag == Tag::User || tag == Tag::Group; }
  std::string permissions(std::uint32_t value) {
    return std::string{(value & 4) != 0 ? 'r' : '-', (value & 2) != 0 ? 'w' : '-', (value & 1) != 0 ? 'x' : '-'};
  }
} // namespace
AclEditor::AclEditor(std::function<void()> changed) : m_changed(std::move(changed)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceSm);
  addChild(ui::label({.text = "Access control", .fontWeight = FontWeight::SemiBold}));
  addChild(
      ui::label(
          {.text = "The mask limits the owning group and named users/groups. Mask changes are explicit.",
           .fontSize = Style::fontSizeCaption,
           .maxLines = 2}
      )
  );
  addChild(
      ui::label({.out = &m_reason, .fontSize = Style::fontSizeCaption, .maxLines = 3, .configure = [](Label& label) {
                   label.setUseMarkup(false);
                 }})
  );
  addChild(ui::column({.out = &m_rows, .align = FlexAlign::Stretch, .gap = Style::spaceMd}));
  addChild(
      ui::label({.out = &m_status, .fontSize = Style::fontSizeCaption, .maxLines = 4, .configure = [](Label& label) {
                   label.setUseMarkup(false);
                 }})
  );
}
void AclEditor::changed(bool layout) {
  m_draft = m_actual && (m_access != m_actual->access || m_defaults != m_actual->defaults);
  m_status->setText(m_draft ? "Unsaved access control changes." : "");
  m_status->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_rebuild |= layout;
  if (!layout) {
    updateEffective();
  }
  m_changed();
}
void AclEditor::sync(const luciole::PermissionSnapshot& snapshot) {
  const auto actual = snapshot.info ? snapshot.info->acl : nullptr;
  const bool preserve = m_draft
      && snapshot.aclOperation
      && (snapshot.phase == luciole::PermissionPhase::Saving || (snapshot.error.has_value() && !snapshot.applied));
  const bool editable = snapshot.phase == luciole::PermissionPhase::Ready && actual && actual->editable;
  if (actual != m_actual || editable != m_editable) {
    if (!preserve) {
      m_access = actual ? actual->access : std::vector<luciole::AclEntry>{};
      m_defaults = actual ? actual->defaults : std::vector<luciole::AclEntry>{};
      m_draft = false;
    }
    m_actual = actual;
    m_editable = editable;
    m_rebuild = true;
  }
  m_reason->setText(actual ? actual->message : "ACL inspection is available for one native regular file or folder.");
  const auto message = snapshot.aclOperation ? permissionStatusText(snapshot) : "";
  m_status->setText(
      snapshot.aclOperation && snapshot.phase == luciole::PermissionPhase::Saving ? "Saving access control…" : message
  );
  m_status->setColor(colorSpecFromRole(snapshot.failed() ? ColorRole::Error : ColorRole::OnSurfaceVariant));
  m_reason->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
}
luciole::AclEdit AclEditor::edit() const {
  luciole::AclEdit value;
  if (m_actual && m_access != m_actual->access) {
    value.access = m_access;
  }
  if (m_actual && m_actual->directory && m_defaults != m_actual->defaults) {
    value.defaults = m_defaults;
  }
  return value;
}
bool AclEditor::canApply() const {
  const auto value = edit();
  return m_editable && (value.access || value.defaults);
}
const Node* AclEditor::messageNode() const { return m_status; }
void AclEditor::updateEffective() {
  for (const auto& [label, key] : m_effective) {
    const auto& rows = key.first ? m_defaults : m_access;
    if (key.second >= rows.size()) {
      continue;
    }
    const auto& row = rows[key.second];
    const auto mask = std::ranges::find(rows, Tag::Mask, &luciole::AclEntry::tag);
    const auto effective = (named(row.tag) || row.tag == Tag::OwningGroup) && mask != rows.end()
        ? row.permissions & mask->permissions
        : row.permissions;
    label->setText("Effective: " + permissions(effective));
  }
}
void AclEditor::recalculate(bool defaults) {
  auto& rows = defaults ? m_defaults : m_access;
  std::uint32_t value = 0;
  for (const auto& row : rows) {
    if (named(row.tag) || row.tag == Tag::OwningGroup) {
      value |= row.permissions;
    }
  }
  auto mask = std::ranges::find(rows, Tag::Mask, &luciole::AclEntry::tag);
  if (mask != rows.end()) {
    mask->permissions = value;
  } else if (rows.size() < luciole::maxAclEntries) {
    rows.push_back({Tag::Mask, {}, value});
  }
  changed(true);
}
void AclEditor::rebuild() {
  m_effective.clear();
  while (!m_rows->children().empty()) {
    m_rows->removeChild(m_rows->children().back().get());
  }
  if (!m_actual || !m_actual->supported) {
    return;
  }
  constexpr std::array names{"Owner", "Named user", "Owning group", "Named group", "Mask (group class)", "Others"};
  for (const bool defaults : {false, true}) {
    if (defaults && !m_actual->directory) {
      continue;
    }
    auto& rows = defaults ? m_defaults : m_access;
    m_rows->addChild(ui::label({.text = defaults ? "Default ACL" : "Access ACL", .fontWeight = FontWeight::SemiBold}));
    if (defaults) {
      m_rows->addChild(
          ui::label(
              {.text = "Defaults affect newly created children. Existing children keep their ACLs.",
               .fontSize = Style::fontSizeCaption,
               .maxLines = 2}
          )
      );
      if (rows.empty()) {
        m_rows->addChild(
            ui::button(
                {.text = "Add default ACL", .enabled = m_editable, .variant = ButtonVariant::Ghost, .onClick = [this] {
                   m_defaults = m_access;
                   changed(true);
                 }}
            )
        );
        continue;
      }
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& rule = rows[i];
      auto heading = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm});
      heading->addChild(
          ui::label({.text = names[static_cast<std::size_t>(rule.tag)], .flexGrow = named(rule.tag) ? 0.0F : 1.0F})
      );
      if (named(rule.tag)) {
        heading->addChild(
            ui::input(
                {.value = rule.qualifier,
                 .placeholder = "Name or numeric ID",
                 .enabled = m_editable,
                 .flexGrow = 1.0F,
                 .onChange = [this, defaults, i](const std::string& value) {
                   (defaults ? m_defaults : m_access)[i].qualifier = value.substr(0, 1025);
                   changed(false);
                 }}
            )
        );
      }
      if (named(rule.tag) || rule.tag == Tag::Mask) {
        const bool removable =
            named(rule.tag) || std::ranges::none_of(rows, [](const auto& row) { return named(row.tag); });
        heading->addChild(
            ui::button(
                {.glyph = "x",
                 .enabled = m_editable && removable,
                 .variant = ButtonVariant::Ghost,
                 .tooltip = "Remove entry",
                 .onClick = [this, defaults, i] {
                   auto& values = defaults ? m_defaults : m_access;
                   values.erase(values.begin() + static_cast<std::ptrdiff_t>(i));
                   changed(true);
                 }}
            )
        );
      }
      auto flags = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm});
      constexpr std::array labels{"Read", "Write", "Execute"};
      for (std::size_t bit = 0; bit < labels.size(); ++bit) {
        const auto value = 4U >> bit;
        flags->addChild(
            ui::toggle(
                {.checkedImmediate = (rule.permissions & value) != 0,
                 .enabled = m_editable,
                 .onChange = [this, defaults, i, value](bool checked) {
                   auto& permissions = (defaults ? m_defaults : m_access)[i].permissions;
                   permissions = checked ? permissions | value : permissions & ~value;
                   changed(false);
                 }}
            )
        );
        flags->addChild(ui::label({.text = labels[bit], .fontSize = Style::fontSizeCaption}));
      }
      Label* effective = nullptr;
      flags->addChild(
          ui::label(
              {.out = &effective,
               .text = "",
               .fontSize = Style::fontSizeCaption,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .flexGrow = 1.0F}
          )
      );
      m_effective.push_back({effective, {defaults, i}});
      m_rows->addChild(
          ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs}, std::move(heading), std::move(flags))
      );
    }
    auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm});
    const bool mask = std::ranges::any_of(rows, [](const auto& row) { return row.tag == Tag::Mask; });
    for (const bool user : {true, false}) {
      actions->addChild(
          ui::button(
              {.text = user ? "Add user" : "Add group",
               .enabled = m_editable && rows.size() + (mask ? 1 : 2) <= luciole::maxAclEntries,
               .variant = ButtonVariant::Ghost,
               .onClick = [this, defaults, user] {
                 auto& values = defaults ? m_defaults : m_access;
                 if (std::ranges::none_of(values, [](const auto& row) { return row.tag == Tag::Mask; })) {
                   const auto group = std::ranges::find(values, Tag::OwningGroup, &luciole::AclEntry::tag);
                   values.push_back({Tag::Mask, {}, group == values.end() ? 0 : group->permissions});
                 }
                 values.push_back({user ? Tag::User : Tag::Group, {}, 0});
                 changed(true);
               }}
          )
      );
    }
    actions->addChild(
        ui::button(
            {.text = "Recalculate mask",
             .enabled = m_editable && (mask || rows.size() < luciole::maxAclEntries),
             .variant = ButtonVariant::Ghost,
             .onClick = [this, defaults] { recalculate(defaults); }}
        )
    );
    m_rows->addChild(std::move(actions));
    if (defaults) {
      m_rows->addChild(
          ui::button(
              {.text = "Remove default ACL", .enabled = m_editable, .variant = ButtonVariant::Ghost, .onClick = [this] {
                 m_defaults.clear();
                 changed(true);
               }}
          )
      );
    }
  }
  updateEffective();
}
void AclEditor::doLayout(Renderer& renderer) {
  prepareRows();
  Flex::doLayout(renderer);
}
void AclEditor::prepareRows() {
  if (m_rebuild) {
    m_rebuild = false;
    rebuild();
  }
}
LayoutSize AclEditor::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  prepareRows();
  return Flex::doMeasure(renderer, constraints);
}
void AclEditor::doArrange(Renderer& renderer, const LayoutRect& rect) {
  prepareRows();
  Flex::doArrange(renderer, rect);
}
