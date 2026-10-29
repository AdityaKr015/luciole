#include "config/state_store.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <toml++/toml.hpp>
#include <unordered_set>
#include <utility>

namespace {
  constexpr std::size_t kMaxWindows = 16;
  constexpr std::size_t kMaxTabs = 64;
  constexpr std::size_t kMaxUriBytes = 64 * 1024;

  std::optional<std::string> uriProblem(const std::string& uri) {
    if (uri.size() > kMaxUriBytes || uri.contains('\0')) {
      return "expected a URI of at most 64 KiB without NUL";
    }
    constexpr auto flags =
        static_cast<GUriFlags>(G_URI_FLAGS_ENCODED | G_URI_FLAGS_HAS_PASSWORD | G_URI_FLAGS_HAS_AUTH_PARAMS);
    const std::unique_ptr<GUri, decltype(&g_uri_unref)> parsed(g_uri_parse(uri.c_str(), flags, nullptr), g_uri_unref);
    if (!parsed) {
      return "expected an absolute URI";
    }
    if (g_uri_get_password(parsed.get()) != nullptr
        || g_uri_get_auth_params(parsed.get()) != nullptr
        || g_uri_get_query(parsed.get()) != nullptr
        || g_uri_get_fragment(parsed.get()) != nullptr) {
      return "stored locations cannot contain passwords, authentication parameters, queries or fragments";
    }
    return std::nullopt;
  }

  files::ConfigIssue problem(const std::filesystem::path& path, std::string key, std::string message) {
    return {.file = path, .key = std::move(key), .message = std::move(message)};
  }

  files::ConfigIssues validate(const files::SessionState& state, const std::filesystem::path& path) {
    files::ConfigIssues issues;
    const auto add = [&](std::string key, std::string message) {
      issues.push_back(problem(path, std::move(key), std::move(message)));
    };
    const auto location = [&](const files::Location& value, const std::string& key) {
      if (const auto error = uriProblem(value.uri())) {
        add(key, *error);
      }
    };
    if (state.bookmarks.size() > files::StateStore::bookmarkLimit) {
      add("bookmarks", "at most 256 bookmarks are supported");
    }
    std::unordered_set<std::string> uris;
    for (std::size_t i = 0; i < state.bookmarks.size(); ++i) {
      const auto& bookmark = state.bookmarks[i];
      const auto key = std::format("bookmarks[{}]", i);
      if (const auto error = files::bookmarkLabelProblem(bookmark.label)) {
        add(key + ".label", *error);
      }
      location(bookmark.location, key + ".uri");
      if (!uris.insert(bookmark.location.uri()).second) {
        add(key + ".uri", "duplicate bookmark location");
      }
    }
    if (state.windows.size() > kMaxWindows) {
      add("windows", "at most 16 windows are supported");
    }
    for (std::size_t i = 0; i < state.windows.size(); ++i) {
      const auto& window = state.windows[i];
      const auto key = std::format("windows[{}]", i);
      if (window.tabs.empty() || window.tabs.size() > kMaxTabs) {
        add(key + ".tabs", "expected between 1 and 64 tabs");
      }
      if (window.activeTab >= window.tabs.size()) {
        add(key + ".active_tab", "active tab is outside the saved tab list");
      }
      for (std::size_t j = 0; j < window.tabs.size(); ++j) {
        const auto& tab = window.tabs[j];
        const auto tabKey = std::format("{}.tabs[{}]", key, j);
        location(tab.primary, tabKey + ".primary_uri");
        if (tab.secondary) {
          location(*tab.secondary, tabKey + ".secondary_uri");
        }
        if ((tab.activePane != files::ActivePane::PRIMARY && tab.activePane != files::ActivePane::SECONDARY)
            || (tab.activePane == files::ActivePane::SECONDARY && !tab.secondary)) {
          add(tabKey + ".active_pane", "active pane must exist and be primary or secondary");
        }
      }
    }
    return issues;
  }

  // Shape checks retain source positions; cross-record checks retain key paths.
  class Decoder {
  public:
    explicit Decoder(std::filesystem::path path) : m_path(std::move(path)) {}
    files::ConfigIssues issues;

    void issue(const toml::node& node, std::string key, std::string message) {
      const auto& position = node.source().begin;
      issues.push_back(
          {.file = m_path,
           .line = position.line,
           .column = position.column,
           .key = std::move(key),
           .message = std::move(message)}
      );
    }

    void keys(const toml::table& table, const std::string& prefix, std::initializer_list<std::string_view> allowed) {
      for (const auto& [key, value] : table) {
        if (std::ranges::find(allowed, key.str()) == allowed.end()) {
          issue(value, prefix + std::string(key.str()), "unknown state field");
        }
      }
    }

    const toml::array*
    array(const toml::table& table, std::string_view field, const std::string& key, std::size_t max, bool required) {
      const auto* value = table.get(field);
      if (value == nullptr && !required) {
        return nullptr;
      }
      if (value == nullptr || !value->is_array() || value->as_array()->size() > max) {
        issue(value != nullptr ? *value : table, key, std::format("expected an array with at most {} items", max));
        return nullptr;
      }
      return value->as_array();
    }

    std::optional<std::string> text(const toml::table& table, std::string_view field, const std::string& key) {
      const auto* node = table.get(field);
      if (node == nullptr || !node->is_string()) {
        issue(node != nullptr ? *node : table, key, "expected a string");
        return std::nullopt;
      }
      return node->as_string()->get();
    }

    std::optional<files::Location> location(const toml::table& table, std::string_view field, const std::string& key) {
      const auto uri = text(table, field, key);
      if (!uri) {
        return std::nullopt;
      }
      if (const auto error = uriProblem(*uri)) {
        issue(*table.get(field), key, *error);
        return std::nullopt;
      }
      try {
        return files::Location::fromUri(*uri);
      } catch (const std::invalid_argument&) {
        issue(*table.get(field), key, "expected an absolute operation URI");
        return std::nullopt;
      }
    }

    std::optional<files::SavedTab> tab(const toml::node& node, const std::string& key) {
      const auto* table = node.as_table();
      if (table == nullptr) {
        issue(node, key, "expected a tab table");
        return std::nullopt;
      }
      keys(*table, key + ".", {"primary_uri", "secondary_uri", "active_pane"});
      auto primary = location(*table, "primary_uri", key + ".primary_uri");
      std::optional<files::Location> secondary;
      if (table->contains("secondary_uri")) {
        secondary = location(*table, "secondary_uri", key + ".secondary_uri");
      }
      const auto active = text(*table, "active_pane", key + ".active_pane");
      if (active && *active != "primary" && *active != "secondary") {
        issue(*table->get("active_pane"), key + ".active_pane", "expected 'primary' or 'secondary'");
      }
      if (!primary || !active) {
        return std::nullopt;
      }
      return files::SavedTab{
          .primary = std::move(*primary),
          .secondary = std::move(secondary),
          .activePane = *active == "secondary" ? files::ActivePane::SECONDARY : files::ActivePane::PRIMARY
      };
    }

    std::optional<files::SavedWindow> window(const toml::node& node, const std::string& key) {
      const auto* table = node.as_table();
      if (table == nullptr) {
        issue(node, key, "expected a window table");
        return std::nullopt;
      }
      keys(*table, key + ".", {"tabs", "active_tab"});
      files::SavedWindow result;
      const auto* active = table->get("active_tab");
      if (active == nullptr
          || !active->is_integer()
          || active->as_integer()->get() < 0
          || active->as_integer()->get() >= static_cast<std::int64_t>(kMaxTabs)) {
        issue(active != nullptr ? *active : node, key + ".active_tab", "expected an integer from 0 to 63");
      } else {
        result.activeTab = static_cast<std::size_t>(active->as_integer()->get());
      }
      if (const auto* tabs = array(*table, "tabs", key + ".tabs", kMaxTabs, true)) {
        for (std::size_t i = 0; i < tabs->size(); ++i) {
          if (auto value = tab(*tabs->get(i), std::format("{}.tabs[{}]", key, i))) {
            result.tabs.push_back(std::move(*value));
          }
        }
      }
      return result;
    }

  private:
    std::filesystem::path m_path;
  };

  std::expected<files::SessionState, files::ConfigIssues>
  parseState(const std::optional<std::string>& text, const std::filesystem::path& path) {
    if (!text) {
      return files::SessionState{};
    }
    toml::table table;
    try {
      table = toml::parse(*text, path.string());
    } catch (const toml::parse_error& error) {
      const auto& at = error.source().begin;
      return std::unexpected(
          files::ConfigIssues{
              {.file = path,
               .line = at.line,
               .column = at.column,
               .key = {},
               .message = std::string(error.description())}
          }
      );
    }
    Decoder decoder(path);
    decoder.keys(table, "", {"schema_version", "places_customized", "bookmarks", "windows"});
    const auto* version = table.get("schema_version");
    if (version == nullptr || !version->is_integer() || version->as_integer()->get() != 1) {
      decoder.issue(version != nullptr ? *version : table, "schema_version", "expected integer state schema version 1");
    }
    files::SessionState state;
    if (const auto* customized = table.get("places_customized")) {
      if (!customized->is_boolean()) {
        decoder.issue(*customized, "places_customized", "expected a boolean");
      } else {
        state.placesCustomized = customized->as_boolean()->get();
      }
    }
    if (const auto* bookmarks =
            decoder.array(table, "bookmarks", "bookmarks", files::StateStore::bookmarkLimit, state.placesCustomized)) {
      for (std::size_t i = 0; i < bookmarks->size(); ++i) {
        const auto key = std::format("bookmarks[{}]", i);
        const auto* bookmark = bookmarks->get(i)->as_table();
        if (bookmark == nullptr) {
          decoder.issue(*bookmarks->get(i), key, "expected a bookmark table");
          continue;
        }
        decoder.keys(*bookmark, key + ".", {"label", "uri"});
        auto label = decoder.text(*bookmark, "label", key + ".label");
        auto location = decoder.location(*bookmark, "uri", key + ".uri");
        if (label && location) {
          state.bookmarks.push_back({.label = std::move(*label), .location = std::move(*location)});
        }
      }
    }
    if (const auto* windows = decoder.array(table, "windows", "windows", kMaxWindows, false)) {
      for (std::size_t i = 0; i < windows->size(); ++i) {
        if (auto window = decoder.window(*windows->get(i), std::format("windows[{}]", i))) {
          state.windows.push_back(std::move(*window));
        }
      }
    }
    if (!decoder.issues.empty()) {
      return std::unexpected(std::move(decoder.issues));
    }
    auto issues = validate(state, path);
    return issues.empty() ? std::expected<files::SessionState, files::ConfigIssues>(std::move(state))
                          : std::unexpected(std::move(issues));
  }

  std::string formatState(const files::SessionState& state) {
    toml::array bookmarks;
    for (const auto& bookmark : state.bookmarks) {
      bookmarks.push_back(toml::table{{"label", bookmark.label}, {"uri", bookmark.location.uri()}});
    }
    toml::array windows;
    for (const auto& window : state.windows) {
      toml::array tabs;
      for (const auto& tab : window.tabs) {
        toml::table value{
            {"primary_uri", tab.primary.uri()},
            {"active_pane", tab.activePane == files::ActivePane::PRIMARY ? "primary" : "secondary"}
        };
        if (tab.secondary) {
          value.insert("secondary_uri", tab.secondary->uri());
        }
        tabs.push_back(std::move(value));
      }
      windows.push_back(
          toml::table{{"active_tab", static_cast<std::int64_t>(window.activeTab)}, {"tabs", std::move(tabs)}}
      );
    }
    const toml::table document{
        {"schema_version", 1},
        {"places_customized", state.placesCustomized},
        {"bookmarks", std::move(bookmarks)},
        {"windows", std::move(windows)}
    };
    // Adapted from Noctalia StateStore's canonical TOML formatting.
    std::ostringstream output;
    output << toml::toml_formatter{
        document, toml::toml_formatter::default_flags & ~toml::format_flags::allow_literal_strings
    };
    auto text = output.str();
    if (!text.ends_with('\n')) {
      text.push_back('\n');
    }
    return text;
  }
} // namespace

std::optional<std::string> files::bookmarkLabelProblem(std::string_view label) {
  if (label.empty()) {
    return "Enter a label.";
  }
  if (label.size() > 256) {
    return "Labels can contain at most 256 UTF-8 bytes.";
  }
  if (!g_utf8_validate(label.data(), static_cast<gssize>(label.size()), nullptr)) {
    return "Labels must be valid UTF-8 text.";
  }
  if (std::ranges::any_of(label, [](unsigned char c) { return c < 32 || c == 127; })) {
    return "Labels cannot contain control characters.";
  }
  return std::nullopt;
}

files::StateStore::StateStore(std::filesystem::path path) : m_path(std::move(path)) {}

std::filesystem::path files::StateStore::pathFromEnvironment() {
  std::filesystem::path root;
  if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && *xdg != '\0') {
    root = xdg;
  } else {
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
      root = std::filesystem::path(home) / ".local/state";
    }
  }
  if (!root.is_absolute()) {
    throw std::invalid_argument("XDG_STATE_HOME, or HOME when unset, must be an absolute path");
  }
  return root / "noctalia-files/state.toml";
}

bool files::StateStore::validPath() const {
  return m_path.is_absolute() && !m_path.native().contains('\0') && m_path.filename() == "state.toml";
}

bool files::StateStore::load() {
  m_issues.clear();
  if (!validPath()) {
    m_issues.push_back(problem(m_path, {}, "expected an absolute state.toml path"));
    return false;
  }
  auto text = readPrivateTextFile(m_path);
  if (!text) {
    m_issues.push_back(problem(m_path, {}, std::move(text.error())));
    return false;
  }
  auto parsed = parseState(*text, m_path);
  if (!parsed) {
    m_issues = std::move(parsed.error());
    return false;
  }
  m_state = std::move(*parsed);
  m_loaded = true;
  return true;
}

files::AtomicWriteState files::StateStore::saveBookmarks(std::vector<Bookmark> bookmarks) {
  auto proposed = m_state;
  proposed.bookmarks = std::move(bookmarks);
  proposed.placesCustomized = true;
  return save(std::move(proposed), true);
}

files::AtomicWriteState files::StateStore::saveWindows(std::vector<SavedWindow> windows) {
  auto proposed = m_state;
  proposed.windows = std::move(windows);
  return save(std::move(proposed), false);
}

files::AtomicWriteState files::StateStore::save(SessionState proposed, bool bookmarks) {
  m_issues.clear();
  if (!m_loaded || !validPath()) {
    m_issues.push_back(problem(m_path, {}, "load a valid state snapshot before saving"));
    return AtomicWriteState::NOT_COMMITTED;
  }
  m_issues = validate(proposed, m_path);
  if (!m_issues.empty()) {
    return AtomicWriteState::NOT_COMMITTED;
  }
  SessionState candidate;
  const auto result = updateTextFileAtomic(
      m_path, [&](const std::optional<std::string>& text) -> std::expected<std::optional<std::string>, std::string> {
        auto current = parseState(text, m_path);
        if (!current) {
          m_issues = std::move(current.error());
          return std::unexpected("invalid existing state; repair or remove it explicitly before saving");
        }
        const bool unchanged = bookmarks
            ? current->bookmarks == proposed.bookmarks && current->placesCustomized == proposed.placesCustomized
            : current->windows == proposed.windows;
        const bool matches = bookmarks
            ? current->bookmarks == m_state.bookmarks && current->placesCustomized == m_state.placesCustomized
            : current->windows == m_state.windows;
        if (!unchanged && !matches) {
          return std::unexpected("state section changed in another writer; reload before retrying");
        }
        candidate = std::move(*current);
        if (bookmarks) {
          candidate.bookmarks = std::move(proposed.bookmarks);
          candidate.placesCustomized = proposed.placesCustomized;
        } else {
          candidate.windows = std::move(proposed.windows);
        }
        if (unchanged) {
          return std::nullopt;
        }
        return formatState(candidate);
      }
  );
  if (result.state != AtomicWriteState::NOT_COMMITTED) {
    m_state = std::move(candidate);
  }
  if (!result.error.empty() && m_issues.empty()) {
    m_issues.push_back(problem(m_path, {}, result.error));
  }
  return result.state;
}
