#pragma once

#include <cstdint>

enum class KeybindAction : std::uint8_t {
  Validate, Cancel, Left, Right, Up, Down, TabNext, TabPrevious, Delete, Copy, Save
};

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

/// Global keybind matchers for UI controls. Set once at application startup.
namespace KeybindMatcher {

  using Matcher = std::function<bool(std::uint32_t sym, std::uint32_t modifiers)>;

  void setMatcher(KeybindAction action, Matcher matcher);
  bool matches(KeybindAction action, std::uint32_t sym, std::uint32_t modifiers);

} // namespace KeybindMatcher
