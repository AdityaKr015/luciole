#include "browser/browser_controller.h"

#include "browser/location_presentation.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <utility>

struct files::BrowserController::ActivationState {
  GioPtr<GCancellable> cancellable{g_cancellable_new()};
  std::function<void(EntryId, const Location&, std::uint64_t, GFileType, const std::string&)> resolved;
};

files::BrowserController::BrowserController(SortOptions sort, bool showHidden)
    : m_activation(std::make_shared<ActivationState>()) {
  m_activation->resolved = [this](
                               EntryId id, const Location& target, std::uint64_t context, GFileType type,
                               const std::string& error
                           ) {
    const auto* entry = m_model.entry(id);
    if (context != m_contextSerial || !entry || entry->location != target || !std::ranges::contains(m_model.rows(), id))
      return;
    if (!error.empty()) {
      showMessage(error);
      return;
    }
    if (type == G_FILE_TYPE_DIRECTORY) {
      open(target);
      return;
    }
    if (m_fileActivated) {
      const auto callback = m_fileActivated;
      callback(target);
    } else
      showMessage("Opening files is not available yet.");
  };
  m_model.setSort(sort);
  m_model.setFilter({.showHidden = showHidden, .text = {}});
  m_model.setChangedCallback([this] {
    if (!m_publishing) {
      m_restoring = false;
      m_restoredViewport.reset();
      notify();
    }
  });
  m_session.setChangedCallback([this] {
    if (!m_switchingSources && m_query.empty()) {
      publish(m_session.snapshot());
    }
  });
  m_search.setChangedCallback([this] {
    if (!m_switchingSources && !m_query.empty()) {
      publish(m_search.snapshot());
    }
  });
}

files::BrowserController::~BrowserController() {
  m_activation->resolved = {};
  g_cancellable_cancel(m_activation->cancellable.get());
  m_session.setChangedCallback({});
  m_search.setChangedCallback({});
  m_model.setChangedCallback({});
}

void files::BrowserController::advanceContext() {
  ++m_contextSerial;
  g_cancellable_cancel(m_activation->cancellable.get());
  m_activation->cancellable.reset(g_cancellable_new());
}

void files::BrowserController::setChangedCallback(std::function<void()> callback) { m_changed = std::move(callback); }
void files::BrowserController::setFileActivatedCallback(std::function<void(const Location&)> callback) {
  m_fileActivated = std::move(callback);
}

void files::BrowserController::setCaptureViewport(std::function<BrowseViewport()> callback) {
  m_captureViewport = std::move(callback);
}
void files::BrowserController::setViewDefaults(ViewMode mode, int iconSize) {
  m_defaults.mode = mode;
  m_defaults.iconSize = iconSize;
}
std::optional<files::BrowseViewport> files::BrowserController::takeRestoredViewport() {
  return std::exchange(m_restoredViewport, std::nullopt);
}
std::optional<files::BrowserController::Reference>
files::BrowserController::reference(std::optional<EntryId> id) const {
  const auto* entry = id ? m_model.entry(*id) : nullptr;
  if (!entry || !entry->identity)
    return std::nullopt;
  return Reference{entry->location.uri(), *entry->identity};
}
void files::BrowserController::saveVisit() {
  if (m_history.empty() || m_restoring)
    return;
  auto& visit = m_history[m_historyIndex];
  visit.query = m_query;
  if (m_captureViewport)
    visit.viewport = m_captureViewport();
  visit.selected.clear();
  visit.selected.reserve(m_model.selection().selected.size());
  for (const auto id : m_model.selectedIds())
    if (auto item = reference(id))
      visit.selected.push_back(std::move(*item));
  visit.focus = reference(m_model.selection().focused);
  visit.anchor = reference(m_model.selection().anchor);
  visit.first = reference(visit.viewport.first);
  visit.viewport.first.reset();
}
void files::BrowserController::loadVisit() {
  const auto destination = m_history[m_historyIndex].location;
  const auto query = m_history[m_historyIndex].query;
  m_viewport = m_history[m_historyIndex].viewport;
  m_restoredViewport.reset();
  m_restoring = true;
  ++m_visitSerial;
  m_message.clear();
  m_query = query;
  advanceContext();
  m_switchingSources = true;
  m_search.cancel();
  m_switchingSources = false;
  if (query.empty()) {
    m_session.open(destination);
  } else {
    // Browse metadata continues loading independently of the search projection.
    m_session.open(destination);
    m_search.start(destination, query, m_model.filter().showHidden);
  }
}
void files::BrowserController::open(const Location& location) {
  const auto destination = location; // NOLINT(performance-unnecessary-copy-initialization)
  if (!m_history.empty() && m_history[m_historyIndex].location == destination) {
    m_restoring = false;
    m_restoredViewport.reset();
    if (!m_query.empty()) {
      m_query.clear();
      advanceContext();
      m_search.cancel();
    }
    refresh();
    return;
  }
  saveVisit();
  if (!m_history.empty())
    m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(m_historyIndex + 1), m_history.end());
  m_history.push_back({destination, {}, m_defaults, {}, {}, {}, {}});
  if (m_history.size() > 128)
    m_history.erase(m_history.begin());
  m_historyIndex = m_history.size() - 1;
  loadVisit();
}

bool files::BrowserController::openText(const std::string& text) {
  if (text.empty()) {
    showMessage("Enter a path or URI.");
    return false;
  }
  try {
    const auto base = m_model.snapshot().location ? *m_model.snapshot().location
                                                  : Location::fromPath(std::filesystem::current_path().native());
    open(resolveLocationText(text, base));
    return true;
  } catch (const std::exception& error) {
    showMessage(error.what());
    return false;
  }
}

void files::BrowserController::home() { (void)openText("~"); }

bool files::BrowserController::canBack() const { return !m_history.empty() && m_historyIndex > 0; }
bool files::BrowserController::canForward() const {
  return !m_history.empty() && m_historyIndex + 1 < m_history.size();
}

bool files::BrowserController::back() {
  if (!canBack())
    return false;
  saveVisit();
  --m_historyIndex;
  loadVisit();
  return true;
}
bool files::BrowserController::forward() {
  if (!canForward())
    return false;
  saveVisit();
  ++m_historyIndex;
  loadVisit();
  return true;
}

bool files::BrowserController::canUp() const {
  if (!m_model.snapshot().location) {
    return false;
  }
  const GioPtr<GFile> parent(g_file_get_parent(m_model.snapshot().location->file().get()));
  return parent != nullptr;
}

bool files::BrowserController::up() {
  if (!m_model.snapshot().location) {
    return false;
  }
  const GioPtr<GFile> parent(g_file_get_parent(m_model.snapshot().location->file().get()));
  if (!parent) {
    return false;
  }
  const GString uri(g_file_get_uri(parent.get()));
  open(Location::fromUri(uri.get()));
  return true;
}

void files::BrowserController::refresh() {
  m_message.clear();
  if (m_query.empty()) {
    m_session.refresh();
  } else if (m_session.snapshot().location) {
    m_search.start(*m_session.snapshot().location, m_query, m_model.filter().showHidden);
  }
}

void files::BrowserController::stop() {
  m_restoring = false;
  m_restoredViewport.reset();
  advanceContext();
  if (m_query.empty()) {
    m_session.cancel();
  } else {
    m_search.cancel();
  }
}

void files::BrowserController::toggleHidden() {
  auto filter = m_model.filter();
  filter.showHidden = !filter.showHidden;
  m_model.setFilter(std::move(filter));
  if (!m_query.empty()) {
    refresh();
  }
}

void files::BrowserController::setSearchQuery(std::string text) {
  if (m_query == text) {
    return;
  }
  m_restoring = false;
  m_restoredViewport.reset();
  advanceContext();
  m_query = std::move(text);
  m_message.clear();
  if (m_query.empty()) {
    m_search.cancel();
    publish(m_session.snapshot());
  } else if (m_session.snapshot().location) {
    m_search.start(*m_session.snapshot().location, m_query, m_model.filter().showHidden);
  }
}

void files::BrowserController::publish(DirectorySnapshot snapshot) {
  snapshot.generation = ++m_revision;
  m_publishing = true;
  m_model.applySnapshot(std::move(snapshot));
  const auto status = m_model.snapshot().status;
  if (m_restoring && (status == DirectoryStatus::READY || status == DirectoryStatus::ERROR)) {
    const auto& visit = m_history[m_historyIndex];
    std::unordered_map<std::string, EntryId> visible;
    visible.reserve(m_model.rows().size());
    for (const auto id : m_model.rows())
      visible.emplace(m_model.entry(id)->location.uri(), id);
    const auto resolve = [&](const std::optional<Reference>& reference) -> std::optional<EntryId> {
      if (!reference)
        return std::nullopt;
      const auto found = visible.find(reference->uri);
      if (found == visible.end() || m_model.entry(found->second)->identity != reference->identity)
        return std::nullopt;
      return found->second;
    };
    SelectionState selection;
    selection.selected.reserve(visit.selected.size());
    for (const auto& item : visit.selected)
      if (const auto id = resolve(item))
        selection.selected.insert(*id);
    selection.focused = resolve(visit.focus);
    selection.anchor = resolve(visit.anchor);
    auto viewport = visit.viewport;
    viewport.first = resolve(visit.first);
    m_model.restoreInteraction(std::move(selection), viewport.first);
    m_restoredViewport = viewport;
    m_restoring = false;
  }
  m_publishing = false;
  notify();
}

void files::BrowserController::activate(EntryId id) {
  const auto* entry = m_model.entry(id);
  if (entry == nullptr || std::ranges::find(m_model.rows(), id) == m_model.rows().end()) {
    return;
  }
  if (entry->location.isTrash()) {
    if (entry->type == G_FILE_TYPE_DIRECTORY && !entry->symlink)
      open(entry->location);
    else
      showMessage("Restore this item before opening it.");
  } else if (entry->symlink) {
    struct Query {
      std::shared_ptr<ActivationState> state;
      EntryId id;
      Location target;
      std::uint64_t context;
    };
    const auto file = entry->location.file();
    g_file_query_info_async(
        file.get(), G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
        m_activation->cancellable.get(),
        [](GObject* source, GAsyncResult* result, gpointer raw) {
          const std::unique_ptr<Query> query(static_cast<Query*>(raw));
          GError* error = nullptr;
          const GioPtr<GFileInfo> info(g_file_query_info_finish(G_FILE(source), result, &error));
          const std::string message = error ? error->message : "";
          const bool cancelled = error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
          g_clear_error(&error);
          const auto callback = query->state->resolved;
          if (callback && !cancelled)
            callback(
                query->id, query->target, query->context,
                info ? g_file_info_get_file_type(info.get()) : G_FILE_TYPE_UNKNOWN, message
            );
        },
        new Query{m_activation, id, entry->location, m_contextSerial}
    );
  } else if (entry->type == G_FILE_TYPE_DIRECTORY) {
    open(entry->location);
  } else if (m_fileActivated) {
    const auto callback = m_fileActivated;
    const auto location = entry->location;
    callback(location);
  } else {
    showMessage("Opening files is not available yet.");
  }
}

void files::BrowserController::showMessage(std::string message) {
  m_message = std::move(message);
  notify();
}

void files::BrowserController::notify() const {
  const auto callback = m_changed;
  if (callback) {
    callback();
  }
}
