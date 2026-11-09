#pragma once

#include "config/settings.h"
#include "ui/controls/flex.h"

#include <functional>
#include <map>

class Label;
class ScrollView;

class BrowserSettingsView final : public Flex {
public:
  BrowserSettingsView(
      std::function<void(files::OverrideEdit)> change, std::function<void()> reset, std::function<void()> close,
      std::function<void()> clearThumbnails
  );
  void sync(
      const files::BrowserSettings& settings, const files::BrowserSettings& inherited, bool busy,
      const std::string& error, bool clearingThumbnails = false
  );
  void reveal(const Node* node);
  void setCompact(bool compact);

private:
  template <class T> using Choices = std::vector<std::pair<std::string, T>>;
  void selectPage(std::size_t selected);
  void section(const std::string& title);
  template <class T> void addChoice(files::PreferenceKey<T> key, const std::string& title, const Choices<T>& choices);
  void addBool(files::PreferenceKey<bool> key, const std::string& title);
  void addNumber(
      files::PreferenceKey<int> key, const std::string& title, const std::vector<int>& values, std::string suffix = "px"
  );
  template <class T>
  void addRow(
      files::PreferenceKey<T> key, const std::string& title, std::function<void(const T&)> show,
      std::unique_ptr<Node> control
  );
  template <class T> void send(files::PreferenceKey<T> key, std::optional<T> value);
  std::function<void(files::OverrideEdit)> m_change;
  files::BrowserSettings m_current;
  files::BrowserSettings m_inherited;
  ScrollView* m_body = nullptr;
  Label* m_status = nullptr;
  class Button* m_clearThumbnails = nullptr;
  Flex* m_navigation = nullptr;
  ScrollView* m_navigationScroll = nullptr;
  Flex* m_section = nullptr;
  std::vector<Flex*> m_pages;
  std::vector<class Button*> m_navButtons;
  // Shows m_current's value in a row's control and whether it overrides m_inherited.
  std::map<files::BrowserPreferenceId, std::function<void()>> m_rows;
};
