#include "config/settings.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <iterator>
#include <optional>
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
    std::optional<bool> sortDescending;
    std::optional<bool> directoriesFirst;
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
          } else {
            std::optional<bool>* target = nullptr;
            if (name == "show_hidden") {
              target = &patch.showHidden;
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

std::string files::ConfigIssue::format() const {
  auto location = file.string();
  if (line != 0) {
    location += std::format(":{}:{}", line, column);
  }
  return key.empty() ? std::format("{}: {}", location, message) : std::format("{}: {}: {}", location, key, message);
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
