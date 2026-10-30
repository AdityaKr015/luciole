#pragma once

#include "browser/directory_model.h"
#include "fs/recursive_search.h"

namespace files {
  class BrowserController {
  public:
    BrowserController(SortOptions sort, bool showHidden);
    ~BrowserController();
    BrowserController(const BrowserController&) = delete;
    BrowserController& operator=(const BrowserController&) = delete;

    void setChangedCallback(std::function<void()> callback);
    void setFileActivatedCallback(std::function<void(const Location&)> callback);
    void open(const Location& location);
    bool openText(const std::string& text);
    void home();
    bool back();
    bool forward();
    bool up();
    void refresh();
    void stop();
    void toggleHidden();
    void setSearchQuery(std::string text);
    void activate(EntryId id);

    [[nodiscard]] std::uint64_t contextSerial() const { return m_contextSerial; }
    [[nodiscard]] bool canBack() const;
    [[nodiscard]] bool canForward() const;
    [[nodiscard]] bool canUp() const;
    [[nodiscard]] const std::string& message() const { return m_message; }
    [[nodiscard]] const std::string& searchQuery() const { return m_query; }
    [[nodiscard]] std::size_t skippedSearchFolders() const { return m_search.skippedFolders(); }
    [[nodiscard]] DirectoryModel& model() { return m_model; }
    [[nodiscard]] const DirectoryModel& model() const { return m_model; }

  private:
    struct ActivationState;
    std::shared_ptr<ActivationState> m_activation;
    void advanceContext();
    void notify() const;
    void publish(DirectorySnapshot snapshot);
    void showMessage(std::string message);
    DirectoryModel m_model;
    DirectorySession m_session;
    RecursiveSearch m_search;
    std::string m_query;
    std::uint64_t m_revision = 0;
    std::uint64_t m_contextSerial = 0;
    std::vector<Location> m_history;
    std::size_t m_historyIndex = 0;
    std::string m_message;
    std::function<void()> m_changed;
    std::function<void(const Location&)> m_fileActivated;
  };
} // namespace files
