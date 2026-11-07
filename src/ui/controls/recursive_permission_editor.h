#pragma once

#include "operations/permission_service.h"
#include "ui/controls/flex.h"

#include <array>

class Input;
class Segmented;

class RecursivePermissionEditor final : public Flex {
public:
  explicit RecursivePermissionEditor(const std::function<void()>& changed);
  files::RecursivePermissionEdit edit() const;
  bool canApply() const;
  void sync(const files::PermissionSnapshot& snapshot);
  const Node* messageNode() const;

private:
  std::array<Segmented*, 12> m_files{}, m_folders{};
  Input *m_owner = nullptr, *m_group = nullptr;
  bool m_ready = false;
};
