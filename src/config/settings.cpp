#include "config/settings.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <toml++/toml.hpp>
#include <unistd.h>
#include <utility>

namespace {
  constexpr std::size_t kMaxConfigBytes = 256 * 1024;

  struct Patch {
    std::optional<bool> showHidden;
    std::optional<files::SortField> sortBy;
    std::optional<files::ViewMode> viewMode;
    std::optional<int> gridIconSize;
    std::optional<int> sidebarWidth;
    std::optional<bool> showSidebar;
    std::optional<bool> sortDescending;
    std::optional<bool> directoriesFirst;
    bool operator==(const Patch&) const = default;
  };

  struct FileDescriptor {
    int fd;
    ~FileDescriptor() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    explicit FileDescriptor(int value) : fd(value) {}
  };

  files::ConfigIssue ioIssue(const std::filesystem::path& file, std::string message) {
    return {.file = file, .key = {}, .message = std::move(message)};
  }

  std::string ioMessage(int error) { return std::error_code(error, std::generic_category()).message(); }

  std::expected<std::optional<std::string>, files::ConfigIssue>
  readFile(const std::filesystem::path& file, bool optional) {
    if (optional) {
      struct stat linkInfo{};
      if (::lstat(file.c_str(), &linkInfo) < 0) {
        const int error = errno;
        if (error == ENOENT) {
          return std::nullopt;
        }
        return std::unexpected(ioIssue(file, ioMessage(error)));
      }
    }
    // Declarative config symlinks are allowed. Nonblocking open plus fstat
    // rejects FIFOs/devices without waiting for a writer or reading them.
    const FileDescriptor descriptor(::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (descriptor.fd < 0) {
      return std::unexpected(ioIssue(file, ioMessage(errno)));
    }
    struct stat info{};
    if (::fstat(descriptor.fd, &info) < 0) {
      return std::unexpected(ioIssue(file, ioMessage(errno)));
    }
    if (!S_ISREG(info.st_mode)) {
      return std::unexpected(ioIssue(file, "expected a regular TOML file"));
    }
    std::string text;
    std::array<char, 8192> buffer{};
    while (true) {
      const auto count = ::read(descriptor.fd, buffer.data(), buffer.size());
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        return std::unexpected(ioIssue(file, ioMessage(errno)));
      }
      if (count == 0) {
        return text;
      }
      const auto size = static_cast<std::size_t>(count);
      if (text.size() + size > kMaxConfigBytes) {
        return std::unexpected(ioIssue(file, "configuration exceeds the 256 KiB limit"));
      }
      text.append(buffer.data(), size);
    }
  }

  void issue(
      files::ConfigIssues& issues, const std::filesystem::path& file, const toml::node& node, std::string key,
      std::string message
  ) {
    const auto& position = node.source().begin;
    issues.push_back(
        {.file = file,
         .line = position.line,
         .column = position.column,
         .key = std::move(key),
         .message = std::move(message)}
    );
  }

  std::expected<Patch, files::ConfigIssues> parse(const std::filesystem::path& file, std::string_view text) {
    toml::table table;
    try {
      table = toml::parse(text, file.string());
    } catch (const toml::parse_error& error) {
      const auto& position = error.source().begin;
      return std::unexpected(
          files::ConfigIssues{
              {.file = file,
               .line = position.line,
               .column = position.column,
               .key = {},
               .message = std::string(error.description())}
          }
      );
    }
    files::ConfigIssues issues;
    Patch patch;
    for (const auto& [key, node] : table) {
      if (key == "schema_version") {
        if (!node.is_integer() || node.as_integer()->get() != 1) {
          issue(issues, file, node, "schema_version", "expected integer schema version 1");
        }
      } else if (key == "browser") {
        const auto* browser = node.as_table();
        if (browser == nullptr) {
          issue(issues, file, node, "browser", "expected a table");
          continue;
        }
        for (const auto& [name, value] : *browser) {
          const auto path = std::format("browser.{}", name.str());
          if (name == "sort_by") {
            const auto* sort = value.as_string();
            if (sort != nullptr && sort->get() == "name") {
              patch.sortBy = files::SortField::NAME;
            } else if (sort != nullptr && sort->get() == "size") {
              patch.sortBy = files::SortField::SIZE;
            } else if (sort != nullptr && sort->get() == "modified") {
              patch.sortBy = files::SortField::MODIFIED;
            } else {
              issue(issues, file, value, path, "expected 'name', 'size', or 'modified'");
            }
          } else if (name == "grid_icon_size") {
            const auto* size = value.as_integer();
            if (size != nullptr && std::ranges::contains(files::kGridIconSizes, size->get())) {
              patch.gridIconSize = static_cast<int>(size->get());
            } else {
              issue(issues, file, value, path, "expected integer icon size 32, 48, 64, 80, 96, 128, 160, 192, or 256");
            }
          } else if (name == "sidebar_width") {
            const auto* width = value.as_integer();
            if (width != nullptr
                && width->get() >= files::kMinSidebarWidth
                && width->get() <= files::kMaxSidebarWidth) {
              patch.sidebarWidth = static_cast<int>(width->get());
            } else {
              issue(issues, file, value, path, "expected integer sidebar width from 160 to 400");
            }
          } else if (name == "view_mode") {
            const auto* mode = value.as_string();
            if (mode != nullptr && mode->get() == "list") {
              patch.viewMode = files::ViewMode::LIST;
            } else if (mode != nullptr && mode->get() == "grid") {
              patch.viewMode = files::ViewMode::GRID;
            } else {
              issue(issues, file, value, path, "expected 'list' or 'grid'");
            }
          } else {
            std::optional<bool>* target = nullptr;
            if (name == "show_hidden") {
              target = &patch.showHidden;
            } else if (name == "show_sidebar") {
              target = &patch.showSidebar;
            } else if (name == "sort_descending") {
              target = &patch.sortDescending;
            } else if (name == "directories_first") {
              target = &patch.directoriesFirst;
            }
            if (target == nullptr) {
              issue(issues, file, value, path, "unknown setting");
            } else if (const auto* boolean = value.as_boolean(); boolean != nullptr) {
              *target = boolean->get();
            } else {
              issue(issues, file, value, path, "expected a boolean");
            }
          }
        }
      } else {
        issue(issues, file, node, std::string(key.str()), "unknown setting");
      }
    }
    if (!issues.empty()) {
      return std::unexpected(std::move(issues));
    }
    return patch;
  }

  void apply(files::Settings& settings, const Patch& patch) {
    if (patch.showSidebar) {
      settings.browser.showSidebar = *patch.showSidebar;
    }
    if (patch.sidebarWidth) {
      settings.browser.sidebarWidth = *patch.sidebarWidth;
    }
    if (patch.gridIconSize) {
      settings.browser.gridIconSize = *patch.gridIconSize;
    }
    if (patch.viewMode) {
      settings.browser.viewMode = *patch.viewMode;
    }
    if (patch.showHidden) {
      settings.browser.showHidden = *patch.showHidden;
    }
    if (patch.sortBy) {
      settings.browser.sort.field = *patch.sortBy;
    }
    if (patch.sortDescending) {
      settings.browser.sort.descending = *patch.sortDescending;
    }
    if (patch.directoriesFirst) {
      settings.browser.sort.directoriesFirst = *patch.directoriesFirst;
    }
  }

  std::string_view preferenceKey(files::BrowserPreference preference) {
    using Preference = files::BrowserPreference;
    switch (preference) {
    case Preference::SHOW_HIDDEN:
      return "browser.show_hidden";
    case Preference::SORT_BY:
      return "browser.sort_by";
    case Preference::SORT_DESCENDING:
      return "browser.sort_descending";
    case Preference::DIRECTORIES_FIRST:
      return "browser.directories_first";
    case Preference::VIEW_MODE:
      return "browser.view_mode";
    case Preference::GRID_ICON_SIZE:
      return "browser.grid_icon_size";
    case Preference::SIDEBAR_WIDTH:
      return "browser.sidebar_width";
    case Preference::SHOW_SIDEBAR:
      return "browser.show_sidebar";
    }
    return {};
  }

  void editPatch(Patch& patch, const files::OverrideEdit& edit) {
    using Preference = files::BrowserPreference;
    if (edit.preference == Preference::SIDEBAR_WIDTH) {
      patch.sidebarWidth = std::holds_alternative<std::monostate>(edit.value)
          ? std::nullopt
          : std::optional<int>(std::get<int>(edit.value));
      return;
    }
    if (edit.preference == Preference::GRID_ICON_SIZE) {
      patch.gridIconSize = std::holds_alternative<std::monostate>(edit.value)
          ? std::nullopt
          : std::optional<int>(std::get<int>(edit.value));
      return;
    }
    if (edit.preference == Preference::VIEW_MODE) {
      patch.viewMode = std::holds_alternative<std::monostate>(edit.value)
          ? std::nullopt
          : std::optional<files::ViewMode>(std::get<files::ViewMode>(edit.value));
      return;
    }
    if (edit.preference == Preference::SORT_BY) {
      patch.sortBy = std::holds_alternative<std::monostate>(edit.value)
          ? std::nullopt
          : std::optional<files::SortField>(std::get<files::SortField>(edit.value));
      return;
    }
    const auto value = std::holds_alternative<std::monostate>(edit.value)
        ? std::nullopt
        : std::optional<bool>(std::get<bool>(edit.value));
    switch (edit.preference) {
    case Preference::SHOW_SIDEBAR:
      patch.showSidebar = value;
      break;
    case Preference::SHOW_HIDDEN:
      patch.showHidden = value;
      break;
    case Preference::SORT_DESCENDING:
      patch.sortDescending = value;
      break;
    case Preference::DIRECTORIES_FIRST:
      patch.directoriesFirst = value;
      break;
    case Preference::SORT_BY:
    case Preference::VIEW_MODE:
    case Preference::GRID_ICON_SIZE:
    case Preference::SIDEBAR_WIDTH:
      break;
    }
  }

  std::string formatPatch(const Patch& patch) {
    toml::table browser;
    if (patch.showSidebar) {
      browser.insert("show_sidebar", *patch.showSidebar);
    }
    if (patch.sidebarWidth) {
      browser.insert("sidebar_width", *patch.sidebarWidth);
    }
    if (patch.gridIconSize) {
      browser.insert("grid_icon_size", *patch.gridIconSize);
    }
    if (patch.viewMode) {
      browser.insert("view_mode", *patch.viewMode == files::ViewMode::LIST ? "list" : "grid");
    }
    if (patch.showHidden) {
      browser.insert("show_hidden", *patch.showHidden);
    }
    if (patch.sortBy) {
      const auto* name = *patch.sortBy == files::SortField::NAME ? "name"
          : *patch.sortBy == files::SortField::SIZE              ? "size"
                                                                 : "modified";
      browser.insert("sort_by", name);
    }
    if (patch.sortDescending) {
      browser.insert("sort_descending", *patch.sortDescending);
    }
    if (patch.directoriesFirst) {
      browser.insert("directories_first", *patch.directoriesFirst);
    }
    toml::table document{{"schema_version", 1}};
    if (!browser.empty()) {
      document.insert("browser", std::move(browser));
    }
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

files::ConfigPaths files::ConfigPaths::fromEnvironment() {
  std::filesystem::path directory;
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
    directory = xdg;
    if (!directory.is_absolute()) {
      throw std::invalid_argument("XDG_CONFIG_HOME must be an absolute path");
    }
  } else {
    const char* home = std::getenv("HOME");
    if (home == nullptr || !std::filesystem::path(home).is_absolute()) {
      throw std::invalid_argument("HOME must be an absolute path when XDG_CONFIG_HOME is unset");
    }
    directory = std::filesystem::path(home) / ".config";
  }
  directory /= "noctalia-files";
  return {.base = directory / "config.toml", .overrides = directory / "settings.toml"};
}

files::ConfigIssues files::validateConfigFile(const std::filesystem::path& file) {
  auto text = readFile(file, false);
  if (!text) {
    return {std::move(text.error())};
  }
  auto parsed = parse(file, **text);
  return parsed ? ConfigIssues{} : std::move(parsed.error());
}

files::SettingsResult files::loadSettings(const ConfigPaths& paths) {
  Settings settings;
  ConfigIssues issues;
  for (const auto* file : {&paths.base, &paths.overrides}) {
    auto text = readFile(*file, true);
    if (!text) {
      issues.push_back(std::move(text.error()));
      continue;
    }
    if (!*text) {
      continue;
    }
    auto parsed = parse(*file, **text);
    if (!parsed) {
      auto& errors = parsed.error();
      issues.insert(issues.end(), std::make_move_iterator(errors.begin()), std::make_move_iterator(errors.end()));
    } else {
      apply(settings, *parsed);
    }
  }
  if (!issues.empty()) {
    return std::unexpected(std::move(issues));
  }
  return settings;
}

files::ConfigService::ConfigService(ConfigPaths paths) : m_paths(std::move(paths)) {}

bool files::ConfigService::reload() {
  auto loaded = loadSettings(m_paths);
  if (!loaded) {
    m_issues = std::move(loaded.error());
    return false;
  }
  m_settings = *loaded;
  m_issues.clear();
  return true;
}

files::AtomicWriteState files::ConfigService::saveOverrides(std::span<const OverrideEdit> edits) {
  if (edits.empty()) {
    return AtomicWriteState::UNCHANGED;
  }
  m_issues.clear();
  if (!m_paths.base.is_absolute()
      || !m_paths.overrides.is_absolute()
      || m_paths.base.filename() != "config.toml"
      || m_paths.overrides.filename() != "settings.toml"
      || m_paths.base.parent_path().lexically_normal() != m_paths.overrides.parent_path().lexically_normal()) {
    m_issues.push_back(
        ioIssue(m_paths.overrides, "saves require distinct config.toml and settings.toml in one absolute directory")
    );
    return AtomicWriteState::NOT_COMMITTED;
  }
  for (const auto& edit : edits) {
    const auto key = preferenceKey(edit.preference);
    bool valid = !key.empty();
    if (!std::holds_alternative<std::monostate>(edit.value)) {
      if (edit.preference == BrowserPreference::SORT_BY) {
        const auto* sort = std::get_if<SortField>(&edit.value);
        valid =
            sort != nullptr && (*sort == SortField::NAME || *sort == SortField::SIZE || *sort == SortField::MODIFIED);
      } else if (edit.preference == BrowserPreference::GRID_ICON_SIZE) {
        const auto* size = std::get_if<int>(&edit.value);
        valid = size != nullptr && std::ranges::contains(kGridIconSizes, *size);
      } else if (edit.preference == BrowserPreference::SIDEBAR_WIDTH) {
        const auto* width = std::get_if<int>(&edit.value);
        valid = width != nullptr && *width >= kMinSidebarWidth && *width <= kMaxSidebarWidth;
      } else if (edit.preference == BrowserPreference::VIEW_MODE) {
        const auto* mode = std::get_if<ViewMode>(&edit.value);
        valid = mode != nullptr && (*mode == ViewMode::LIST || *mode == ViewMode::GRID);
      } else {
        valid = valid && std::holds_alternative<bool>(edit.value);
      }
    }
    if (!valid) {
      m_issues.push_back(
          {.file = m_paths.overrides, .key = std::string(key), .message = "invalid preference or value type"}
      );
    }
  }
  if (!m_issues.empty()) {
    return AtomicWriteState::NOT_COMMITTED;
  }
  Settings candidate;
  const auto result = updateTextFileAtomic(
      m_paths.overrides,
      [&](const std::optional<std::string>& current) -> std::expected<std::optional<std::string>, std::string> {
        std::error_code aliasError;
        if (std::filesystem::equivalent(m_paths.base, m_paths.overrides, aliasError)) {
          return std::unexpected("base configuration and GUI overrides refer to the same file");
        }
        if (aliasError && aliasError != std::errc::no_such_file_or_directory) {
          return std::unexpected(std::format("cannot inspect base configuration: {}", aliasError.message()));
        }
        const auto base = readFile(m_paths.base, true);
        if (!base) {
          m_issues.push_back(base.error());
          return std::unexpected("cannot load base configuration");
        }
        if (*base) {
          const auto parsed = parse(m_paths.base, **base);
          if (!parsed) {
            m_issues = parsed.error();
            return std::unexpected("invalid base configuration");
          }
          apply(candidate, *parsed);
        }
        auto patch = parse(m_paths.overrides, current.value_or(""));
        if (!patch) {
          m_issues = std::move(patch.error());
          return std::unexpected("invalid existing settings");
        }
        const auto previous = *patch;
        for (const auto& edit : edits) {
          editPatch(*patch, edit);
        }
        apply(candidate, *patch);
        if (*patch == previous) {
          return std::nullopt;
        }
        return formatPatch(*patch);
      }
  );
  if (result.state != AtomicWriteState::NOT_COMMITTED) {
    m_settings = candidate;
  }
  if (!result.error.empty() && m_issues.empty()) {
    m_issues.push_back(ioIssue(m_paths.overrides, result.error));
  }
  return result.state;
}
