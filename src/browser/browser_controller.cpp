#include "browser/browser_controller.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <utility>

files::BrowserController::BrowserController(SortOptions sort, bool showHidden) {
  m_model.setSort(sort);
  m_model.setFilter({.showHidden = showHidden, .text = {}});
  m_model.setChangedCallback([this] { notify(); });
  m_session.setChangedCallback([this] { m_model.applySnapshot(m_session.snapshot()); });
}

files::BrowserController::~BrowserController() {
  m_session.setChangedCallback({});
  m_model.setChangedCallback({});
}

void files::BrowserController::setChangedCallback(std::function<void()> callback) { m_changed = std::move(callback); }

void files::BrowserController::open(const Location& location) {
  // Opening publishes a new snapshot and invalidates locations borrowed from the model.
  const auto destination = location; // NOLINT(performance-unnecessary-copy-initialization)
  if (!m_history.empty() && m_history[m_historyIndex] == destination) {
    refresh();
    return;
  }
  if (m_history.empty() || m_history[m_historyIndex] != destination) {
    if (!m_history.empty()) {
      m_history.erase(m_history.begin() + static_cast<std::ptrdiff_t>(m_historyIndex + 1), m_history.end());
    }
    m_history.push_back(destination);
    if (m_history.size() > 128) {
      m_history.erase(m_history.begin());
    }
    m_historyIndex = m_history.size() - 1;
  }
  m_message.clear();
  setNameFilter({});
  m_session.open(destination);
}

bool files::BrowserController::openText(const std::string& text) {
  if (text.empty()) {
    showMessage("Enter a path or URI.");
    return false;
  }
  try {
    std::string expanded = text;
    if (text == "~" || text.starts_with("~/")) {
      const char* home = std::getenv("HOME");
      if (home == nullptr || !g_path_is_absolute(home)) {
        throw std::invalid_argument("HOME must be an absolute path");
      }
      expanded = std::string(home) + text.substr(1);
    }
    if (g_path_is_absolute(expanded.c_str())) {
      open(Location::fromPath(expanded));
    } else if (const GString scheme(g_uri_parse_scheme(expanded.c_str())); scheme) {
      open(Location::fromUri(expanded));
    } else {
      if (expanded.contains('\0')) {
        throw std::invalid_argument("locations cannot contain a NUL byte");
      }
      const auto base = m_model.snapshot().location ? *m_model.snapshot().location
                                                    : Location::fromPath(std::filesystem::current_path().native());
      const GioPtr<GFile> file(g_file_resolve_relative_path(base.file().get(), expanded.c_str()));
      const GString uri(g_file_get_uri(file.get()));
      open(Location::fromUri(uri.get()));
    }
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
  if (!canBack()) {
    return false;
  }
  --m_historyIndex;
  m_message.clear();
  setNameFilter({});
  const auto destination = m_history[m_historyIndex];
  m_session.open(destination);
  return true;
}

bool files::BrowserController::forward() {
  if (!canForward()) {
    return false;
  }
  ++m_historyIndex;
  m_message.clear();
  setNameFilter({});
  const auto destination = m_history[m_historyIndex];
  m_session.open(destination);
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
  m_session.refresh();
}

void files::BrowserController::stop() { m_session.cancel(); }

void files::BrowserController::toggleHidden() {
  auto filter = m_model.filter();
  filter.showHidden = !filter.showHidden;
  m_model.setFilter(std::move(filter));
}

void files::BrowserController::setNameFilter(std::string text) {
  auto filter = m_model.filter();
  if (filter.text == text) {
    return;
  }
  filter.text = std::move(text);
  m_message.clear();
  m_model.setFilter(std::move(filter));
}

void files::BrowserController::activate(EntryId id) {
  const auto* entry = m_model.entry(id);
  if (entry == nullptr || std::ranges::find(m_model.rows(), id) == m_model.rows().end()) {
    return;
  }
  if (entry->type == G_FILE_TYPE_DIRECTORY || entry->symlink) {
    open(entry->location);
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
