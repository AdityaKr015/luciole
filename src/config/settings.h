#pragma once

#include "browser/directory_options.h"

#include <expected>
#include <filesystem>
#include <vector>

namespace files {
  struct BrowserSettings {
    SortOptions sort;
    bool showHidden = false;
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

  struct ConfigIssue {
    std::filesystem::path file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string key;
    std::string message;
    [[nodiscard]] std::string format() const;
  };

  using ConfigIssues = std::vector<ConfigIssue>;
  using SettingsResult = std::expected<Settings, ConfigIssues>;

  // Bounded, read-only local file loading. Call outside Render and traversal jobs.
  // Validation reads exactly one required file, without adjacent override files.
  [[nodiscard]] ConfigIssues validateConfigFile(const std::filesystem::path& file);
  [[nodiscard]] SettingsResult loadSettings(const ConfigPaths& paths);

  class ConfigService {
  public:
    explicit ConfigService(ConfigPaths paths);
    bool reload(); // Explicit reload; failure retains the last valid settings.
    [[nodiscard]] const Settings& settings() const { return m_settings; }
    [[nodiscard]] const ConfigIssues& issues() const { return m_issues; }

  private:
    ConfigPaths m_paths;
    Settings m_settings;
    ConfigIssues m_issues;
  };
} // namespace files
