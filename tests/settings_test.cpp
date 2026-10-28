#include "browser/directory_model.h"
#include "config/settings.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <print>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  void require(bool condition, const std::string& message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  struct Fixture {
    std::filesystem::path directory;
    Fixture() {
      const files::GString name(g_dir_make_tmp("noctalia-settings-test-XXXXXX", nullptr));
      require(name != nullptr, "cannot create settings fixture");
      directory = name.get();
    }
    ~Fixture() {
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void write(const std::string& name, const std::string& text) const {
      std::ofstream file(directory / name, std::ios::binary);
      file << text;
      require(file.good(), "cannot write settings fixture");
    }
    [[nodiscard]] files::ConfigPaths paths() const {
      return {.base = directory / "config.toml", .overrides = directory / "settings.toml"};
    }
  };

  std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }

  void testSchemaAndDiagnostics() {
    Fixture fixture;
    fixture.write(
        "config.toml",
        "schema_version = 1\n[browser]\nshow_hidden = true\nsort_by = 'modified'\n"
        "sort_descending = true\ndirectories_first = false\nview_mode = 'grid'\n"
    );
    require(files::validateConfigFile(fixture.paths().base).empty(), "valid settings rejected");
    fixture.write("settings.toml", "invalid adjacent override");
    require(files::validateConfigFile(fixture.paths().base).empty(), "single-file validation read overrides");
    fixture.write("config.toml", "unexpected = 1\n[browser]\nshow_hidden = 1\nsort_by = 'date'\nunknown = false\n");
    const auto errors = files::validateConfigFile(fixture.paths().base);
    require(errors.size() == 4, "not all unknown/type/enum errors were reported");
    bool foundLine = false;
    for (const auto& error : errors) {
      require(error.file == fixture.paths().base && error.line > 0 && error.column > 0, "diagnostic lost source");
      if (error.key == "browser.show_hidden") {
        foundLine = error.line == 3 && error.format().contains("config.toml:3:") && error.message.contains("boolean");
      }
    }
    require(foundLine, "type diagnostic lost key or source line");
    for (const auto& invalid :
         {"schema_version = 2", "schema_version = true", "schema_version = 1.0", "browser = false",
          "[browser]\nshow_hidden = 'true'", "[browser]\nsort_by = 1", "[browser]\nview_mode = true",
          "[browser]\nview_mode = 'icons'", "[browser]\nshow_hidden = true\nshow_hidden = false", "broken = [",
          "theme = {}"}) {
      fixture.write("config.toml", invalid);
      require(!files::validateConfigFile(fixture.paths().base).empty(), "invalid TOML or schema accepted");
    }
    fixture.write("config.toml", "");
    require(files::validateConfigFile(fixture.paths().base).empty(), "empty partial settings rejected");
  }

  void testLayersAndReload() {
    Fixture fixture;
    files::ConfigService config(fixture.paths());
    require(config.reload() && config.settings() == files::Settings{}, "missing optional layers lost defaults");
    fixture.write(
        "config.toml", "[browser]\nshow_hidden = true\nsort_by = 'size'\nsort_descending = true\nview_mode = 'grid'\n"
    );
    require(
        config.reload() && config.settings().browser.viewMode == files::ViewMode::GRID, "base grid preference was lost"
    );
    fixture.write("settings.toml", "[browser]\nshow_hidden = false\ndirectories_first = false\nview_mode = 'list'\n");
    require(config.reload(), "valid layered settings rejected");
    const auto& browser = config.settings().browser;
    require(
        !browser.showHidden
            && browser.sort.field == files::SortField::SIZE
            && browser.sort.descending
            && !browser.sort.directoriesFirst
            && browser.viewMode == files::ViewMode::LIST,
        "defaults/base/override precedence is wrong"
    );
    const auto lastGood = config.settings();
    fixture.write("settings.toml", "[browser]\nview_mode = 'unknown'\n");
    require(
        !config.reload() && config.settings() == lastGood && !config.issues().empty(), "failed reload changed settings"
    );
    fixture.write("config.toml", "[browser]\nshow_hidden = 'not-a-boolean'\n");
    fixture.write("settings.toml", "[browser]\nshow_hidden = true\n");
    require(!config.reload() && config.settings() == lastGood, "override masked an invalid base file");
    std::filesystem::remove(fixture.paths().base);
    std::filesystem::remove(fixture.paths().overrides);
    require(
        config.reload() && config.issues().empty() && config.settings() == files::Settings{},
        "removed files did not reset defaults"
    );
  }

  void testFilesAndLimits() {
    Fixture fixture;
    require(!files::validateConfigFile(fixture.paths().base).empty(), "missing required file accepted");
    fixture.write("target.toml", "[browser]\nshow_hidden = true\n");
    const auto target = fixture.directory / "target.toml";
    std::filesystem::create_symlink(target, fixture.paths().base);
    require(::chmod(target.c_str(), 0444) == 0, "cannot make declarative fixture read-only");
    files::ConfigService config(fixture.paths());
    const auto original = read(target);
    require(config.reload() && config.settings().browser.showHidden, "read-only declarative symlink rejected");
    require(read(target) == original && std::filesystem::is_symlink(fixture.paths().base), "loading rewrote a source");
    std::filesystem::remove(fixture.paths().base);
    std::filesystem::create_symlink(fixture.directory / "missing-target", fixture.paths().base);
    require(!config.reload(), "dangling config symlink treated as missing optional file");
    std::filesystem::remove(fixture.paths().base);
    std::filesystem::create_directory(fixture.paths().base);
    require(!config.reload(), "configuration directory accepted as a file");
    std::filesystem::remove(fixture.paths().base);
    require(::mkfifo(fixture.paths().base.c_str(), 0600) == 0, "cannot create FIFO fixture");
    require(!config.reload(), "FIFO accepted as configuration");
    std::filesystem::remove(fixture.paths().base);
    std::string bounded(256 * 1024, '#');
    fixture.write("config.toml", bounded);
    require(config.reload(), "exactly bounded valid configuration rejected");
    bounded.push_back('#');
    fixture.write("config.toml", bounded);
    require(!config.reload() && config.issues().front().message.contains("256 KiB"), "oversized config accepted");
    if (::geteuid() != 0) {
      fixture.write("config.toml", "");
      require(::chmod(fixture.paths().base.c_str(), 0000) == 0, "cannot set permission fixture");
      const bool loaded = config.reload();
      ::chmod(fixture.paths().base.c_str(), 0600);
      require(!loaded, "unreadable configuration accepted");
    }
  }

  void testEnvironmentPaths() {
    const char* previous = std::getenv("XDG_CONFIG_HOME");
    const std::optional<std::string> saved = previous != nullptr ? std::optional<std::string>(previous) : std::nullopt;
    ::setenv("XDG_CONFIG_HOME", "/tmp/noctalia-settings-path-test", 1);
    const auto paths = files::ConfigPaths::fromEnvironment();
    require(
        paths.base == "/tmp/noctalia-settings-path-test/noctalia-files/config.toml"
            && paths.overrides == "/tmp/noctalia-settings-path-test/noctalia-files/settings.toml",
        "XDG paths are not canonical"
    );
    ::setenv("XDG_CONFIG_HOME", "relative-invalid-path", 1);
    bool rejected = false;
    try {
      (void)files::ConfigPaths::fromEnvironment();
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected, "relative XDG_CONFIG_HOME silently resolved");
    ::unsetenv("XDG_CONFIG_HOME");
    const auto fallback = files::ConfigPaths::fromEnvironment();
    require(
        fallback.base == std::filesystem::path(std::getenv("HOME")) / ".config/noctalia-files/config.toml",
        "default XDG config path is wrong"
    );
    if (saved) {
      ::setenv("XDG_CONFIG_HOME", saved->c_str(), 1);
    }
  }

  void testBrowserDefaults() {
    Fixture fixture;
    fixture.write(
        "config.toml",
        "[browser]\nshow_hidden = true\nsort_by = 'size'\nsort_descending = true\n"
        "directories_first = false\n"
    );
    const auto loaded = files::loadSettings(fixture.paths());
    require(loaded.has_value(), "cannot load model defaults");
    files::DirectoryModel model;
    model.setSort(loaded->browser.sort);
    model.setFilter({.showHidden = loaded->browser.showHidden, .text = {}});
    files::DirectorySnapshot data;
    data.location = files::Location::fromPath(fixture.directory.native());
    data.generation = 1;
    data.status = files::DirectoryStatus::READY;
    data.entries = {
        {.id = 1,
         .location = files::Location::fromPath((fixture.directory / ".hidden").native()),
         .name = ".hidden",
         .displayName = ".hidden",
         .hidden = true,
         .size = 20,
         .modifiedSeconds = std::nullopt,
         .etag = std::nullopt},
        {.id = 2,
         .location = files::Location::fromPath((fixture.directory / "visible").native()),
         .name = "visible",
         .displayName = "visible",
         .size = 5,
         .modifiedSeconds = std::nullopt,
         .etag = std::nullopt},
    };
    model.applySnapshot(std::move(data));
    require(model.rows() == std::vector<files::EntryId>{1, 2}, "typed settings did not configure model projection");
  }

  void testOverrideSaving() {
    using Preference = files::BrowserPreference;
    using State = files::AtomicWriteState;
    Fixture fixture;
    fixture.write("base-target.toml", "[browser]\nshow_hidden = false\nsort_by = 'size'\n");
    const auto baseTarget = fixture.directory / "base-target.toml";
    std::filesystem::create_symlink(baseTarget, fixture.paths().base);
    require(::chmod(baseTarget.c_str(), 0444) == 0, "cannot protect declarative base fixture");
    const auto baseContent = read(baseTarget);
    files::ConfigService first(fixture.paths());
    files::ConfigService second(fixture.paths());
    require(first.reload() && second.reload(), "cannot load save fixtures");
    const std::array show{files::OverrideEdit{Preference::SHOW_HIDDEN, true}};
    const std::array descending{files::OverrideEdit{Preference::SORT_DESCENDING, true}};
    require(
        first.saveOverrides(show) == State::DURABLE && first.settings().browser.showHidden,
        "override did not persist or update effective settings"
    );
    // The second instance still has its earlier cache. Saving another key must
    // reread the sidecar under the lock rather than overwrite the first edit.
    require(second.saveOverrides(descending) == State::DURABLE, "second preference save failed");
    require(
        second.settings().browser.showHidden
            && second.settings().browser.sort.descending
            && second.settings().browser.sort.field == files::SortField::SIZE,
        "unrelated override was lost"
    );
    require(files::validateConfigFile(fixture.paths().overrides).empty(), "saved sidecar violates schema");
    const auto saved = read(fixture.paths().overrides);
    require(
        second.saveOverrides(descending) == State::UNCHANGED && read(fixture.paths().overrides) == saved,
        "unchanged preference was rewritten"
    );
    const std::array disable{
        files::OverrideEdit{Preference::DIRECTORIES_FIRST, false},
        files::OverrideEdit{Preference::SORT_DESCENDING, false},
    };
    require(
        second.saveOverrides(disable) == State::DURABLE
            && second.settings().browser.showHidden
            && !second.settings().browser.sort.directoriesFirst
            && !second.settings().browser.sort.descending,
        "false-valued batch edits were lost or changed another preference"
    );
    const std::array inherit{files::OverrideEdit{Preference::SHOW_HIDDEN, std::monostate{}}};
    require(
        second.saveOverrides(inherit) == State::DURABLE && !second.settings().browser.showHidden,
        "reset did not restore base inheritance"
    );
    require(!read(fixture.paths().overrides).contains("show_hidden"), "reset left an explicit override");
    require(
        read(baseTarget) == baseContent && std::filesystem::is_symlink(fixture.paths().base),
        "saving modified declarative base configuration"
    );

    const auto lastGood = second.settings();
    fixture.write("settings.toml", "[browser]\nunknown = true\n");
    const auto invalid = read(fixture.paths().overrides);
    require(
        second.saveOverrides(show) == State::NOT_COMMITTED
            && second.settings() == lastGood
            && read(fixture.paths().overrides) == invalid
            && !second.issues().empty(),
        "invalid existing sidecar was overwritten or published"
    );
    std::filesystem::remove(fixture.paths().overrides);
    std::filesystem::create_symlink(baseTarget, fixture.paths().overrides);
    require(
        second.saveOverrides(show) == State::NOT_COMMITTED && read(baseTarget) == baseContent,
        "save followed a settings symlink"
    );
    std::filesystem::remove(fixture.paths().overrides);
    const std::array invalidEdits{files::OverrideEdit{Preference::SORT_BY, true}};
    require(
        second.saveOverrides(invalidEdits) == State::NOT_COMMITTED
            && !std::filesystem::exists(fixture.paths().overrides),
        "mistyped preference created an override"
    );
    const std::array sort{files::OverrideEdit{Preference::SORT_BY, files::SortField::MODIFIED}};
    require(
        second.saveOverrides(sort) == State::DURABLE
            && second.settings().browser.sort.field == files::SortField::MODIFIED,
        "typed sort override failed"
    );
    const std::array grid{files::OverrideEdit{Preference::VIEW_MODE, files::ViewMode::GRID}};
    require(second.saveOverrides(grid) == State::DURABLE, "grid preference did not persist");
    files::ConfigService reloaded(fixture.paths());
    require(
        reloaded.reload()
            && reloaded.settings().browser.viewMode == files::ViewMode::GRID
            && reloaded.settings().browser.sort.field == files::SortField::MODIFIED,
        "view preference roundtrip lost a typed value or unrelated key"
    );
    const auto gridBytes = read(fixture.paths().overrides);
    const std::array invalidView{files::OverrideEdit{Preference::VIEW_MODE, true}};
    const std::array invalidMode{files::OverrideEdit{Preference::VIEW_MODE, static_cast<files::ViewMode>(99)}};
    require(
        second.saveOverrides(invalidView) == State::NOT_COMMITTED
            && second.saveOverrides(invalidMode) == State::NOT_COMMITTED
            && read(fixture.paths().overrides) == gridBytes,
        "invalid view preference changed the sidecar"
    );
    const std::array resetView{files::OverrideEdit{Preference::VIEW_MODE, std::monostate{}}};
    require(
        second.saveOverrides(resetView) == State::DURABLE
            && second.settings().browser.viewMode == files::ViewMode::LIST
            && !read(fixture.paths().overrides).contains("view_mode"),
        "view reset did not restore inheritance"
    );
    files::ConfigService aliased({.base = fixture.paths().base, .overrides = fixture.paths().base});
    require(
        aliased.saveOverrides(show) == State::NOT_COMMITTED && read(baseTarget) == baseContent,
        "save accepted the base path as its override destination"
    );
    Fixture reverseAlias;
    reverseAlias.write("settings.toml", "[browser]\nshow_hidden = false\n");
    const auto aliasedContent = read(reverseAlias.paths().overrides);
    std::filesystem::create_symlink(reverseAlias.paths().overrides, reverseAlias.paths().base);
    files::ConfigService sameFile(reverseAlias.paths());
    require(
        sameFile.saveOverrides(show) == State::NOT_COMMITTED && read(reverseAlias.paths().overrides) == aliasedContent,
        "base symlink alias was modified indirectly"
    );

    Fixture invalidBase;
    invalidBase.write("config.toml", "[browser]\nshow_hidden = true\n");
    files::ConfigService protectedConfig(invalidBase.paths());
    require(protectedConfig.saveOverrides(descending) == State::DURABLE, "cannot create invalid-base fixture");
    const auto before = read(invalidBase.paths().overrides);
    const auto beforeSettings = protectedConfig.settings();
    invalidBase.write("config.toml", "schema_version = 999\n");
    require(
        protectedConfig.saveOverrides(sort) == State::NOT_COMMITTED
            && protectedConfig.settings() == beforeSettings
            && read(invalidBase.paths().overrides) == before,
        "invalid base allowed a save or changed the published settings"
    );
  }
} // namespace

int main() {
  try {
    testSchemaAndDiagnostics();
    testLayersAndReload();
    testFilesAndLimits();
    testEnvironmentPaths();
    testBrowserDefaults();
    testOverrideSaving();
    std::println("settings checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "settings test: {}", error.what());
    return 1;
  }
}
