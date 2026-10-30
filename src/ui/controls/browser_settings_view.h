#pragma once

#include "config/settings.h"
#include "ui/controls/flex.h"

#include <functional>
#include <map>

class Label;
class ScrollView;

class BrowserSettingsView final : public Flex {
public:
  using Value = decltype(files::OverrideEdit::value);
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
  using Read = std::function<Value(const files::BrowserSettings&)>;
  using Choice = std::pair<std::string, Value>;
  void selectPage(std::size_t selected);
  void section(const std::string& title);
  void addChoice(files::BrowserPreference key, const std::string& title, Read read, const std::vector<Choice>& choices);
  void addBool(files::BrowserPreference key, const std::string& title, Read read);
  void addNumber(
      files::BrowserPreference key, const std::string& title, Read read, const std::vector<int>& values,
      std::string suffix = "px"
  );
  void addRow(files::BrowserPreference key, const std::string& title, Read read, std::unique_ptr<Node> control);
  void send(files::BrowserPreference key, Value value);
  std::function<void(files::OverrideEdit)> m_change;
  files::BrowserSettings m_inherited;
  ScrollView* m_body = nullptr;
  Label* m_status = nullptr;
  class Button* m_clearThumbnails = nullptr;
  Flex* m_navigation = nullptr;
  Flex* m_section = nullptr;
  std::vector<Flex*> m_pages;
  std::vector<class Button*> m_navButtons;
  std::map<files::BrowserPreference, Read> m_reads;
  std::map<files::BrowserPreference, Value> m_values;
  std::map<files::BrowserPreference, std::function<void(const Value&)>> m_apply;
  std::map<files::BrowserPreference, class Button*> m_resets;
};
