#pragma once

#include "browser/action_registry.h"
#include "core/input/key_chord.h"

#include <cstdint>
#include <initializer_list>
#include <optional>

namespace files {
  // The key chord parsed from the action's shortcut string, or nullopt when it has none.
  const std::optional<KeyChord>& actionChord(BrowserAction action);
  bool shortcutMatches(BrowserAction action, std::uint32_t sym, std::uint32_t modifiers);
  // The first of `actions` whose shortcut matches the key.
  std::optional<BrowserAction>
  shortcutAction(std::initializer_list<BrowserAction> actions, std::uint32_t sym, std::uint32_t modifiers);
} // namespace files
