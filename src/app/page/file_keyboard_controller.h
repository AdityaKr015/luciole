#pragma once

#include "browser/directory_model.h"
#include "browser/selection_tools.h"
#include "core/timer_manager.h"
#include "model/directory_options.h"
#include "wayland/text_input_service.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace files {
  class MillerColumns;
} // namespace files

struct KeyboardEvent;
class LocationBarController;
class MenuController;
class PageContext;
class StatusBarController;

// Keyboard use of the file list: moving the focus and selection, the list's menu and command keys, typeahead
// selection, and the text input client that feeds typed text to typeahead or the search field.
class FileKeyboardController final : public TextInputClient {
public:
  FileKeyboardController(
      PageContext& page, TextInputService& textInput, files::MillerColumns& miller, MenuController& menus,
      LocationBarController& locationBar, StatusBarController& status
  );

  void setTypingMode(files::TypingMode mode) { m_typingMode = mode; }
  // True while the list shows the keyboard focus ring.
  bool keyboardFocus() const { return m_fileKeyboardFocus; }
  void setKeyboardFocus(bool focus) { m_fileKeyboardFocus = focus; }
  // The item a key moved to, which the next sync scrolls into view.
  std::optional<files::EntryId> takeReveal() { return std::exchange(m_keyboardReveal, std::nullopt); }
  void resetReveal() { m_keyboardReveal.reset(); }

  // Typed text, including the input method's preedit; empty while typeahead is idle.
  std::string typeaheadText() const { return m_typeahead.text() + m_typeaheadPreedit; }
  void clearTypeahead();
  // BrowserPage::syncBrowser(): ends typeahead once the folder or its selection changed.
  void syncContext();
  // Reports the client state to the input method, while the list has focus.
  void notifyStateChanged();

  // Keyboard hooks in BrowserPage::keyboard(), in this order.
  // Escape ends typeahead.
  bool escapeKey(const KeyboardEvent& event);
  // Swallows keys while the input method composes typeahead text.
  bool preeditKey(const KeyboardEvent& event);
  // Menu, Delete, F2 and Alt+Return on the focused list.
  bool commandKey(const KeyboardEvent& event);
  // Selection and focus movement on the focused list.
  bool listKey(const KeyboardEvent& event);
  // Printable keys: typeahead, or opening the search field.
  bool typeKey(const KeyboardEvent& event);

  TextInputState textInputState() const override;
  void textInputApplyEdit(const TextInputEdit& edit) override;
  void textInputResetPreedit() override;
  void textInputActivated(TextInputService&) override;
  void textInputDeactivated(TextInputService&) override;

private:
  bool listFocused() const;
  void typeaheadChanged(std::optional<files::EntryId> match, TextInputChangeCause cause = TextInputChangeCause::Other);
  void applyTypeahead(
      const std::string& text, TextInputChangeCause cause = TextInputChangeCause::Other, bool composing = false
  );

  PageContext& m_page;
  TextInputService& m_textInput;
  files::MillerColumns& m_miller;
  MenuController& m_menus;
  LocationBarController& m_locationBar;
  StatusBarController& m_status;
  files::TypingMode m_typingMode = files::TypingMode::Search;
  files::TypeaheadSelection m_typeahead;
  std::string m_typeaheadPreedit;
  Timer m_typeaheadTimer;
  std::uint64_t m_typeaheadContext = 0;
  bool m_fileKeyboardFocus = false;
  std::optional<files::EntryId> m_keyboardReveal;
};
