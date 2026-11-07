#pragma once

#include "operations/permission_service.h"
#include "ui/controls/flex.h"

#include <array>

class Input;
class Toggle;
class Label;
class Segmented;
class RecursivePermissionEditor;

class PermissionEditor final : public Flex {
public:
  explicit PermissionEditor(std::function<void()> edited, bool multiple = false, bool folders = false);
  void sync(const files::PermissionSnapshot& snapshot);
  bool canApply() const;
  files::PermissionEdit edit() const;
  bool recursive() const;
  files::RecursivePermissionEdit recursiveEdit() const;
  const Node* messageNode() const;

private:
  void changed();
  std::uint32_t mode() const;
  std::array<Toggle*, 12> m_bits{};
  std::array<Segmented*, 12> m_choices{};
  std::array<Label*, 12> m_current{};
  bool m_multiple = false;
  Toggle* m_recursive = nullptr;
  Flex* m_direct = nullptr;
  RecursivePermissionEditor* m_tree = nullptr;
  Input *m_owner = nullptr, *m_group = nullptr;
  Label *m_mode = nullptr, *m_reason = nullptr, *m_message = nullptr, *m_ids = nullptr;
  files::PermissionSnapshot m_snapshot;
  std::string m_initialOwner, m_initialGroup;
  bool m_syncing = false;
  std::function<void()> m_edited;
};
