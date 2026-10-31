#pragma once

#include "browser/directory_model.h"
#include "fs/recursive_search.h"

namespace files {
  struct BrowseViewport {
    ViewMode mode = ViewMode::GRID;
    int iconSize = 48;
    float vertical = 0;
    float horizontal = 0;
    float withinRow = 0;
    std::optional<EntryId> first = std::nullopt;
  };

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
    void setCaptureViewport(std::function<BrowseViewport()> callback);
    void setViewDefaults(ViewMode mode, int iconSize);
    void setDefaultViewMode(ViewMode mode) {
      m_defaults.mode = mode;
      m_restoring = false;
      m_restoredViewport.reset();
    }
    void setDefaultIconSize(int size) {
      m_defaults.iconSize = size;
      m_restoring = false;
      m_restoredViewport.reset();
    }
    [[nodiscard]] const BrowseViewport& viewport() const { return m_viewport; }
    [[nodiscard]] std::uint64_t visitSerial() const { return m_visitSerial; }
    [[nodiscard]] std::optional<BrowseViewport> takeRestoredViewport();
    void duplicateFrom(BrowserController& source);
    void home();
    bool back();
    bool forward();
    bool up();
    void refresh();
    void stop();
    void toggleHidden();
    // Programmatic/history changes apply immediately; editor changes use the trailing debounce.
    void setSearchQuery(std::string text);
    void editSearchQuery(std::string text);
    void submitSearchQuery();
    void setSearchComposing(bool composing);
    [[nodiscard]] bool searchPending() const { return m_searchPending; }
    void activate(EntryId id);

    [[nodiscard]] std::uint64_t contextSerial() const { return m_contextSerial; }
    [[nodiscard]] bool canBack() const;
    [[nodiscard]] bool canForward() const;
    [[nodiscard]] bool canUp() const;
    [[nodiscard]] const std::string& message() const { return m_message; }
    [[nodiscard]] const std::string& searchQuery() const { return m_query; }
    [[nodiscard]] std::size_t skippedSearchFolders() const { return m_searchActive ? m_search.skippedFolders() : 0; }
    [[nodiscard]] DirectoryModel& model() { return m_model; }
    [[nodiscard]] const DirectoryModel& model() const { return m_model; }

  private:
    struct ActivationState;
    std::shared_ptr<ActivationState> m_activation;
    void advanceContext();
    void notify() const;
    void publish(DirectorySnapshot snapshot);
    void showMessage(std::string message);
    void cancelSearchTimeout();
    void scheduleSearch();
    void startSearch();
    void publishPendingSearch();
    DirectoryModel m_model;
    DirectorySession m_session;
    RecursiveSearch m_search;
    std::string m_query;
    GSource* m_searchTimeout = nullptr;
    bool m_searchPending = false;
    bool m_searchComposing = false;
    bool m_searchActive = false;
    std::uint64_t m_revision = 0;
    std::uint64_t m_contextSerial = 0;
    struct Reference {
      std::string uri;
      FileIdentity identity;
    };
    struct Visit {
      Location location;
      std::string query;
      BrowseViewport viewport;
      std::vector<Reference> selected;
      std::optional<Reference> focus;
      std::optional<Reference> anchor;
      std::optional<Reference> first;
    };
    void saveVisit();
    void loadVisit();
    [[nodiscard]] std::optional<Reference> reference(std::optional<EntryId> id) const;
    std::vector<Visit> m_history;
    BrowseViewport m_defaults;
    BrowseViewport m_viewport;
    std::optional<BrowseViewport> m_restoredViewport;
    std::function<BrowseViewport()> m_captureViewport;
    std::uint64_t m_visitSerial = 0;
    bool m_restoring = false;
    bool m_publishing = false;
    bool m_switchingSources = false;
    std::size_t m_historyIndex = 0;
    std::string m_message;
    std::function<void()> m_changed;
    std::function<void(const Location&)> m_fileActivated;
  };
} // namespace files
