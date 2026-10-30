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
    std::optional<files::ActivationMode> activationMode;
    std::optional<files::TypingMode> typingMode;
    std::optional<bool> showHidden;
    std::optional<files::SortField> sortBy;
    std::optional<files::ViewMode> viewMode;
    std::optional<int> gridIconSize;
    std::optional<int> sidebarWidth;
    std::optional<bool> showSidebar;
    std::optional<bool> sortDescending;
    std::optional<bool> directoriesFirst;
    std::optional<bool> showTypeColumn;
    std::optional<bool> showSizeColumn;
    std::optional<bool> showModifiedColumn;
    std::optional<files::DetailsColumnOrder> columnOrder;
    std::array<std::optional<int>, 4> columnWidths;
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
          const auto* widthKey = std::ranges::find(files::kDetailsColumnWidthKeys, name.str());
          if (name == "activation_mode") {
            const auto* mode = value.as_string();
            if (mode && (mode->get() == "single_click" || mode->get() == "double_click"))
              patch.activationMode = mode->get() == "single_click" ? files::ActivationMode::SINGLE_CLICK
                                                                   : files::ActivationMode::DOUBLE_CLICK;
            else
              issue(issues, file, value, path, "expected 'single_click' or 'double_click'");
          } else if (name == "typing_mode") {
            const auto* mode = value.as_string();
            if (mode && (mode->get() == "search" || mode->get() == "typeahead"))
              patch.typingMode = mode->get() == "search" ? files::TypingMode::SEARCH : files::TypingMode::TYPEAHEAD;
            else
              issue(issues, file, value, path, "expected 'search' or 'typeahead'");
          } else if (widthKey != files::kDetailsColumnWidthKeys.end()) {
            const auto index = static_cast<std::size_t>(widthKey - files::kDetailsColumnWidthKeys.begin());
            const auto* width = value.as_integer();
            if (width != nullptr
                && width->get() <= files::maximumDetailsColumnWidth(index)
                && width->get() >= 0
                && files::validDetailsColumnWidth(index, static_cast<int>(width->get())))
              patch.columnWidths[index] = static_cast<int>(width->get());
            else
              issue(
                  issues, file, value, path,
                  index == 0 ? "expected 0 (automatic) or an integer width from 160 to 16384"
                             : "expected an integer width from 64 to 2048"
              );
          } else if (name == "column_order") {
            const auto* columns = value.as_array();
            files::DetailsColumnOrder order{};
            bool valid = columns != nullptr && columns->size() == order.size();
            if (valid) {
              for (std::size_t index = 0; index < order.size(); ++index) {
                const auto* text = columns->get(index)->as_string();
                const auto column = text != nullptr ? files::detailsColumnFromName(text->get()) : std::nullopt;
                if (!column) {
                  valid = false;
                  break;
                }
                order[index] = *column;
              }
            }
            if (valid && files::validDetailsColumnOrder(order))
              patch.columnOrder = order;
            else
              issue(
                  issues, file, value, path, "expected an array containing 'type', 'size', and 'modified' exactly once"
              );
          } else if (name == "sort_by") {
            const auto* sort = value.as_string();
            if (sort != nullptr && sort->get() == "name") {
              patch.sortBy = files::SortField::NAME;
            } else if (sort != nullptr && sort->get() == "size") {
              patch.sortBy = files::SortField::SIZE;
            } else if (sort != nullptr && sort->get() == "modified") {
              patch.sortBy = files::SortField::MODIFIED;
            } else if (sort != nullptr && sort->get() == "type") {
              patch.sortBy = files::SortField::TYPE;
            } else {
              issue(issues, file, value, path, "expected 'name', 'size', 'modified', or 'type'");
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
            } else if (name == "show_type_column") {
              target = &patch.showTypeColumn;
            } else if (name == "show_size_column") {
              target = &patch.showSizeColumn;
            } else if (name == "show_modified_column") {
              target = &patch.showModifiedColumn;
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
    if (patch.activationMode)
      settings.browser.activationMode = *patch.activationMode;
    if (patch.typingMode)
      settings.browser.typingMode = *patch.typingMode;
    for (std::size_t index = 0; index < patch.columnWidths.size(); ++index)
      if (patch.columnWidths[index])
        settings.browser.columnWidths[index] = *patch.columnWidths[index];
    if (patch.columnOrder)
      settings.browser.columnOrder = *patch.columnOrder;
    if (patch.showTypeColumn)
      settings.browser.columns.type = *patch.showTypeColumn;
    if (patch.showSizeColumn)
      settings.browser.columns.size = *patch.showSizeColumn;
    if (patch.showModifiedColumn)
      settings.browser.columns.modified = *patch.showModifiedColumn;
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

  std::optional<std::size_t> columnWidthIndex(files::BrowserPreference preference) {
    using P = files::BrowserPreference;
    switch (preference) {
    case P::NAME_COLUMN_WIDTH:
      return 0;
    case P::TYPE_COLUMN_WIDTH:
      return 1;
    case P::SIZE_COLUMN_WIDTH:
      return 2;
    case P::MODIFIED_COLUMN_WIDTH:
      return 3;
    default:
      return std::nullopt;
    }
  }

  std::string_view preferenceKey(files::BrowserPreference preference) {
    using Preference = files::BrowserPreference;
    switch (preference) {
    case Preference::ACTIVATION_MODE:
      return "browser.activation_mode";
    case Preference::TYPING_MODE:
      return "browser.typing_mode";
    case Preference::NAME_COLUMN_WIDTH:
      return "browser.name_column_width";
    case Preference::TYPE_COLUMN_WIDTH:
      return "browser.type_column_width";
    case Preference::SIZE_COLUMN_WIDTH:
      return "browser.size_column_width";
    case Preference::MODIFIED_COLUMN_WIDTH:
      return "browser.modified_column_width";
    case Preference::COLUMN_ORDER:
      return "browser.column_order";
    case Preference::SHOW_TYPE_COLUMN:
      return "browser.show_type_column";
    case Preference::SHOW_SIZE_COLUMN:
      return "browser.show_size_column";
    case Preference::SHOW_MODIFIED_COLUMN:
      return "browser.show_modified_column";
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
    if (edit.preference == Preference::ACTIVATION_MODE) {
      patch.activationMode = std::holds_alternative<std::monostate>(edit.value)
          ? std::nullopt
          : std::optional(std::get<files::ActivationMode>(edit.value));
      return;
    }
    if (edit.preference == Preference::TYPING_MODE) {
      patch.typingMode = std::holds_alternative<std::monostate>(edit.value)
          ? std::nullopt
          : std::optional(std::get<files::TypingMode>(edit.value));
      return;
    }
    if (const auto index = columnWidthIndex(edit.preference)) {
      patch.columnWidths[*index] = std::holds_alternative<std::monostate>(edit.value)
          ? std::nullopt
          : std::optional<int>(std::get<int>(edit.value));
      return;
    }
    if (edit.preference == Preference::COLUMN_ORDER) {
      patch.columnOrder = std::holds_alternative<std::monostate>(edit.value)
          ? std::nullopt
          : std::optional<files::DetailsColumnOrder>(std::get<files::DetailsColumnOrder>(edit.value));
      return;
    }
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
    case Preference::SHOW_TYPE_COLUMN:
      patch.showTypeColumn = value;
      break;
    case Preference::SHOW_SIZE_COLUMN:
      patch.showSizeColumn = value;
      break;
    case Preference::SHOW_MODIFIED_COLUMN:
      patch.showModifiedColumn = value;
      break;
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
    case Preference::ACTIVATION_MODE:
    case Preference::TYPING_MODE:
    case Preference::NAME_COLUMN_WIDTH:
    case Preference::TYPE_COLUMN_WIDTH:
    case Preference::SIZE_COLUMN_WIDTH:
    case Preference::MODIFIED_COLUMN_WIDTH:
    case Preference::SORT_BY:
    case Preference::COLUMN_ORDER:
    case Preference::VIEW_MODE:
    case Preference::GRID_ICON_SIZE:
    case Preference::SIDEBAR_WIDTH:
      break;
    }
  }

  std::string formatPatch(const Patch& patch) {
    toml::table browser;
    if (patch.activationMode)
      browser.insert(
          "activation_mode",
          *patch.activationMode == files::ActivationMode::SINGLE_CLICK ? "single_click" : "double_click"
      );
    if (patch.typingMode)
      browser.insert("typing_mode", *patch.typingMode == files::TypingMode::SEARCH ? "search" : "typeahead");
    for (std::size_t index = 0; index < patch.columnWidths.size(); ++index)
      if (patch.columnWidths[index])
        browser.insert(files::kDetailsColumnWidthKeys[index], *patch.columnWidths[index]);
    if (patch.columnOrder) {
      toml::array columns;
      for (const auto column : *patch.columnOrder)
        columns.push_back(files::detailsColumnName(column));
      browser.insert("column_order", std::move(columns));
    }
    if (patch.showTypeColumn)
      browser.insert("show_type_column", *patch.showTypeColumn);
    if (patch.showSizeColumn)
      browser.insert("show_size_column", *patch.showSizeColumn);
    if (patch.showModifiedColumn)
      browser.insert("show_modified_column", *patch.showModifiedColumn);
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
          : *patch.sortBy == files::SortField::TYPE              ? "type"
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

static files::SettingsResult loadLayers(const files::ConfigPaths& paths, bool overrides) {
  files::Settings settings;
  files::ConfigIssues issues;
  for (const auto* file : {&paths.base, &paths.overrides}) {
    if (file == &paths.overrides && !overrides)
      continue;
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

files::SettingsResult files::loadSettings(const ConfigPaths& paths) { return loadLayers(paths, true); }
files::SettingsResult files::loadInheritedSettings(const ConfigPaths& paths) { return loadLayers(paths, false); }

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
      if (edit.preference == BrowserPreference::ACTIVATION_MODE) {
        const auto* mode = std::get_if<ActivationMode>(&edit.value);
        valid = mode && (*mode == ActivationMode::SINGLE_CLICK || *mode == ActivationMode::DOUBLE_CLICK);
      } else if (edit.preference == BrowserPreference::TYPING_MODE) {
        const auto* mode = std::get_if<TypingMode>(&edit.value);
        valid = mode && (*mode == TypingMode::SEARCH || *mode == TypingMode::TYPEAHEAD);
      } else if (const auto index = columnWidthIndex(edit.preference)) {
        const auto* width = std::get_if<int>(&edit.value);
        valid = width != nullptr && validDetailsColumnWidth(*index, *width);
      } else if (edit.preference == BrowserPreference::COLUMN_ORDER) {
        const auto* order = std::get_if<DetailsColumnOrder>(&edit.value);
        valid = order != nullptr && validDetailsColumnOrder(*order);
      } else if (edit.preference == BrowserPreference::SORT_BY) {
        const auto* sort = std::get_if<SortField>(&edit.value);
        valid = sort != nullptr
            && (*sort == SortField::NAME
                || *sort == SortField::SIZE
                || *sort == SortField::MODIFIED
                || *sort == SortField::TYPE);
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
