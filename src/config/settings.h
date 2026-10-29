#pragma once

#include "browser/directory_options.h"
#include "config/atomic_file.h"
#include "config/diagnostic.h"

#include <expected>
#include <filesystem>
#include <span>
#include <variant>
#include <vector>

namespace files {
  struct BrowserSettings {
    SortOptions sort;
    ViewMode viewMode = ViewMode::LIST;
    bool showHidden = false;
    int gridIconSize = kDefaultGridIconSize;
    int sidebarWidth = kDefaultSidebarWidth;
    bool showSidebar = true;
    DetailsColumnVisibility columns{};
    bool operator==(const BrowserSettings&) const = default;
  };

  struct Settings {
    BrowserSettings browser;
    bool operator==(const Settings&) const = default;
  };

  struct ConfigPaths {
    std::filesystem::path base;
    std::filesystem::path overrides;
    static ConfigPaths fromEnvironment();
  };

  using SettingsResult = std::expected<Settings, ConfigIssues>;

  enum class BrowserPreference : std::uint8_t {
    SHOW_HIDDEN,
    SORT_BY,
    SORT_DESCENDING,
    DIRECTORIES_FIRST,
    VIEW_MODE,
    GRID_ICON_SIZE,
    SIDEBAR_WIDTH,
    SHOW_SIDEBAR,
    SHOW_TYPE_COLUMN,
    SHOW_SIZE_COLUMN,
    SHOW_MODIFIED_COLUMN
  };
  struct OverrideEdit {
    BrowserPreference preference;
    std::variant<std::monostate, bool, SortField, ViewMode, int>
        value; // monostate restores inheritance from the base/defaults.
  };

  // Bounded, read-only local file loading. Call outside Render and traversal jobs.
  // Validation reads exactly one required file, without adjacent override files.
  [[nodiscard]] ConfigIssues validateConfigFile(const std::filesystem::path& file);
  [[nodiscard]] SettingsResult loadSettings(const ConfigPaths& paths);

  class ConfigService {
  public:
    explicit ConfigService(ConfigPaths paths);
    bool reload(); // Explicit reload; failure retains the last valid settings.
    // Single-owner service: perform saves on its I/O thread and marshal results
    // to UI owners. Never call concurrently on the same service or during Render.
    AtomicWriteState saveOverrides(std::span<const OverrideEdit> edits);
    [[nodiscard]] const Settings& settings() const { return m_settings; }
    [[nodiscard]] const ConfigIssues& issues() const { return m_issues; }

  private:
    ConfigPaths m_paths;
    Settings m_settings;
    ConfigIssues m_issues;
  };
} // namespace files
