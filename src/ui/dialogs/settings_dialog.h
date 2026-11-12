#pragma once

#include "config/settings.h"
#include "ui/controls/flex.h"

#include <functional>
#include <map>

class Label;
class ScrollView;

class SettingsDialog final : public Flex {
public:
  SettingsDialog(
      std::function<void(luciole::OverrideEdit)> change, std::function<void()> reset, std::function<void()> close,
      std::function<void()> clearThumbnails
  );
  void sync(
      const luciole::BrowserSettings& settings, const luciole::BrowserSettings& inherited, bool busy,
      const std::string& error, bool clearingThumbnails = false
  );
  void reveal(const Node* node);
  void setCompact(bool compact);

private:
  template <class T> using Choices = std::vector<std::pair<std::string, T>>;
  void selectPage(std::size_t selected);
  void section(const std::string& title, const std::string& glyph);
  // Caption heading for a group of rows within the current section.
  void group(const std::string& title);
  template <class T> void addChoice(luciole::PreferenceKey<T> key, const std::string& title, const Choices<T>& choices);
  void addTerminal();
  void addBool(luciole::PreferenceKey<bool> key, const std::string& title);
  void addNumber(
      luciole::PreferenceKey<int> key, const std::string& title, const std::vector<int>& values,
      std::string suffix = "px"
  );
  template <class T>
  void addRow(
      luciole::PreferenceKey<T> key, const std::string& title, std::function<void(const T&)> show,
      std::unique_ptr<Node> control
  );
  template <class T> void send(luciole::PreferenceKey<T> key, std::optional<T> value);
  std::function<void(luciole::OverrideEdit)> m_change;
  luciole::BrowserSettings m_current;
  luciole::BrowserSettings m_inherited;
  ScrollView* m_body = nullptr;
  Label* m_status = nullptr;
  class Button* m_clearThumbnails = nullptr;
  Flex* m_navigation = nullptr;
  ScrollView* m_navigationScroll = nullptr;
  Flex* m_section = nullptr;
  std::vector<Flex*> m_pages;
  std::vector<class Button*> m_navButtons;
  // Shows m_current's value in a row's control.
  std::map<luciole::BrowserPreferenceId, std::function<void()>> m_rows;
};
