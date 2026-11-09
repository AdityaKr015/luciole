#include "app/page/active_dialog.h"

#include <utility>

void ActiveDialog::open(std::unique_ptr<Flex> root) {
  m_root = std::move(root);
  m_kind = Kind::Menu;
}

void ActiveDialog::bind(Kind kind, std::function<void()> onClose) {
  m_kind = kind;
  m_onClose = std::move(onClose);
}

void ActiveDialog::close() {
  ++m_generation;
  m_kind = Kind::None;
  if (auto onClose = std::exchange(m_onClose, {})) {
    onClose();
  }
}

void ActiveDialog::release() { m_root.reset(); }
