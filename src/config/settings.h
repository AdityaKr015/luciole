#pragma once

#include "browser/directory_options.h"
#include "browser/thumbnail_options.h"
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
    ActivationMode activationMode = ActivationMode::DOUBLE_CLICK;
    TypingMode typingMode = TypingMode::SEARCH;
    ViewMode viewMode = ViewMode::LIST;
    bool showHidden = false;
    int gridIconSize = kDefaultGridIconSize;
    int sidebarWidth = kDefaultSidebarWidth;
    bool showSidebar = true;
    bool showPreview = true;
    int previewWidth = 320;
    DetailsColumnVisibility columns{};
    DetailsColumnOrder columnOrder = kDefaultDetailsColumnOrder;
    DetailsColumnWidths columnWidths = kDefaultDetailsColumnWidths;
    ThumbnailOptions thumbnails;
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
    ACTIVATION_MODE,
    TYPING_MODE,
    SHOW_HIDDEN,
    SORT_BY,
    SORT_DESCENDING,
    DIRECTORIES_FIRST,
    VIEW_MODE,
    GRID_ICON_SIZE,
    SIDEBAR_WIDTH,
    SHOW_SIDEBAR,
    SHOW_PREVIEW,
    PREVIEW_WIDTH,
    SHOW_TYPE_COLUMN,
    SHOW_SIZE_COLUMN,
    SHOW_MODIFIED_COLUMN,
    COLUMN_ORDER,
    NAME_COLUMN_WIDTH,
    TYPE_COLUMN_WIDTH,
    SIZE_COLUMN_WIDTH,
    MODIFIED_COLUMN_WIDTH,
    THUMBNAIL_POLICY,
    THUMBNAIL_MAX_FILE_MIB,
    THUMBNAIL_MAX_MEGAPIXELS,
    THUMBNAIL_CACHE_MIB,
    COUNT
  };
  struct OverrideEdit {
    BrowserPreference preference;
    std::variant<
        std::monostate, bool, SortField, ViewMode, int, DetailsColumnOrder, ActivationMode, TypingMode, ThumbnailPolicy>
        value; // monostate restores inheritance from the base/defaults.
  };

  // Bounded, read-only local file loading. Call outside Render and traversal jobs.
  // Validation reads exactly one required file, without adjacent override files.
  [[nodiscard]] ConfigIssues validateConfigFile(const std::filesystem::path& file);
  [[nodiscard]] SettingsResult loadSettings(const ConfigPaths& paths);
  [[nodiscard]] SettingsResult loadInheritedSettings(const ConfigPaths& paths);

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
