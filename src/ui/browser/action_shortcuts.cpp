#include "ui/browser/action_shortcuts.h"

#include "core/input/key_modifiers.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <xkbcommon/xkbcommon.h>

namespace {
  using Chords = std::array<std::optional<KeyChord>, files::kBrowserActionCount>;

  std::optional<KeyChord> parseShortcut(std::string_view shortcut) {
    if (shortcut.empty()) {
      return std::nullopt;
    }
    std::string spec(shortcut);
    // The table shows the glyph; xkb names the key "comma".
    if (spec.ends_with("+,")) {
      spec.replace(spec.size() - 1, 1, "comma");
    }
    auto chord = parseKeyChordSpec(spec);
    if (chord) {
      chord->sym = xkb_keysym_to_lower(chord->sym);
    }
    return chord;
  }

  const Chords& chords() {
    static const Chords table = [] {
      Chords parsed;
      for (const auto& definition : files::ActionRegistry::definitions()) {
        parsed[std::to_underlying(definition.action)] = parseShortcut(definition.shortcut);
      }
      return parsed;
    }();
    return table;
  }
} // namespace

const std::optional<KeyChord>& files::actionChord(BrowserAction action) { return chords()[std::to_underlying(action)]; }

bool files::shortcutMatches(BrowserAction action, std::uint32_t sym, std::uint32_t modifiers) {
  const auto& chord = actionChord(action);
  // Shift turns letter keysyms upper case, so shifted chords accept either case.
  return chord
      && chord->modifiers == modifiers
      && (chord->sym == sym || ((modifiers & KeyMod::Shift) != 0 && xkb_keysym_to_lower(sym) == chord->sym));
}

std::optional<files::BrowserAction>
files::shortcutAction(std::initializer_list<BrowserAction> actions, std::uint32_t sym, std::uint32_t modifiers) {
  for (const auto action : actions) {
    if (shortcutMatches(action, sym, modifiers)) {
      return action;
    }
  }
  return std::nullopt;
}
