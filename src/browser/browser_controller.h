#pragma once

#include "browser/directory_model.h"

namespace files {
  class BrowserController {
  public:
    BrowserController(SortOptions sort, bool showHidden);
    ~BrowserController();
    BrowserController(const BrowserController&) = delete;
    BrowserController& operator=(const BrowserController&) = delete;

    void setChangedCallback(std::function<void()> callback);
    void open(const Location& location);
    bool openText(const std::string& text);
    void home();
    bool back();
    bool forward();
    bool up();
    void refresh();
    void stop();
    void toggleHidden();
    void setNameFilter(std::string text);
    void activate(EntryId id);

    [[nodiscard]] bool canBack() const;
    [[nodiscard]] bool canForward() const;
    [[nodiscard]] bool canUp() const;
    [[nodiscard]] const std::string& message() const { return m_message; }
    [[nodiscard]] DirectoryModel& model() { return m_model; }
    [[nodiscard]] const DirectoryModel& model() const { return m_model; }

  private:
    void notify() const;
    void showMessage(std::string message);
    DirectoryModel m_model;
    DirectorySession m_session;
    std::vector<Location> m_history;
    std::size_t m_historyIndex = 0;
    std::string m_message;
    std::function<void()> m_changed;
  };
} // namespace files
