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
    ActivationMode activationMode = ActivationMode::DoubleClick;
    TypingMode typingMode = TypingMode::Search;
    ViewMode viewMode = ViewMode::List;
    bool showHidden = false;
    int gridIconSize = kDefaultGridIconSize;
    int sidebarWidth = kDefaultSidebarWidth;
    bool showSidebar = true;
    bool showPreview = true;
    int previewWidth = 320;
    int millerColumnWidth = 260;
    bool millerShowParents = true;
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
    ActivationMode,
    TypingMode,
    ShowHidden,
    SortBy,
    SortDescending,
    DirectoriesFirst,
    ViewMode,
    GridIconSize,
    SidebarWidth,
    ShowSidebar,
    ShowPreview,
    PreviewWidth,
    MillerColumnWidth,
    MillerShowParents,
    ShowTypeColumn,
    ShowSizeColumn,
    ShowModifiedColumn,
    ColumnOrder,
    NameColumnWidth,
    TypeColumnWidth,
    SizeColumnWidth,
    ModifiedColumnWidth,
    ThumbnailPolicy,
    ThumbnailMaxFileMib,
    ThumbnailMaxMegapixels,
    ThumbnailCacheMib,
    Count
  };
  struct OverrideEdit {
    BrowserPreference preference;
    std::variant<
        std::monostate, bool, SortField, ViewMode, int, DetailsColumnOrder, ActivationMode, TypingMode, ThumbnailPolicy>
        value; // monostate restores inheritance from the base/defaults.
  };

  // Bounded, read-only local file loading. Call outside Render and traversal jobs.
  // Validation reads exactly one required file, without adjacent override files.
  ConfigIssues validateConfigFile(const std::filesystem::path& file);
  SettingsResult loadSettings(const ConfigPaths& paths);
  SettingsResult loadInheritedSettings(const ConfigPaths& paths);

  class ConfigService {
  public:
    explicit ConfigService(ConfigPaths paths);
    bool reload(); // Explicit reload; failure retains the last valid settings.
    // Single-owner service: perform saves on its I/O thread and marshal results
    // to UI owners. Never call concurrently on the same service or during Render.
    AtomicWriteState saveOverrides(std::span<const OverrideEdit> edits);
    const Settings& settings() const { return m_settings; }
    const ConfigIssues& issues() const { return m_issues; }

  private:
    ConfigPaths m_paths;
    Settings m_settings;
    ConfigIssues m_issues;
  };
} // namespace files
