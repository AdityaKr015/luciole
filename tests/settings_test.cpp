#include "browser/directory_model.h"
#include "config/settings.h"

#include <algorithm>
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
    fixture.write("config.toml", "[browser]\nsort_by = 'type'\n");
    std::filesystem::remove(fixture.paths().overrides);
    const auto typed = files::loadSettings(fixture.paths());
    require(typed && typed->browser.sort.field == files::SortField::TYPE, "type sort configuration rejected");
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
         .etag = std::nullopt,
         .parentDisplayName = {}},
        {.id = 2,
         .location = files::Location::fromPath((fixture.directory / "visible").native()),
         .name = "visible",
         .displayName = "visible",
         .size = 5,
         .modifiedSeconds = std::nullopt,
         .etag = std::nullopt,
         .parentDisplayName = {}},
    };
    model.applySnapshot(std::move(data));
    require(model.rows() == std::vector<files::EntryId>{1, 2}, "typed settings did not configure model projection");
  }

  void testGridIconSize() {
    Fixture fixture;
    using Preference = files::BrowserPreference;
    using State = files::AtomicWriteState;
    for (const int size : files::kGridIconSizes) {
      fixture.write("config.toml", "[browser]\ngrid_icon_size = " + std::to_string(size) + "\n");
      const auto loaded = files::loadSettings(fixture.paths());
      require(loaded && loaded->browser.gridIconSize == size, "valid grid icon size rejected");
    }
    for (const auto* value : {"0", "31", "49", "129", "257", "-32", "64.0", "true", "'64'", "9223372036854775807"}) {
      fixture.write("config.toml", std::string("[browser]\ngrid_icon_size = ") + value + "\n");
      require(!files::validateConfigFile(fixture.paths().base).empty(), "invalid grid icon size accepted");
    }
    fixture.write("config.toml", "[browser]\ngrid_icon_size = 64\n");
    fixture.write("settings.toml", "[browser]\ngrid_icon_size = 96\nshow_hidden = true\n");
    files::ConfigService config(fixture.paths());
    require(config.reload() && config.settings().browser.gridIconSize == 96, "zoom layering ignored the override");
    for (const int size : {32, 256}) {
      const std::array zoom{files::OverrideEdit{Preference::GRID_ICON_SIZE, size}};
      require(config.saveOverrides(zoom) == State::DURABLE, "zoom preference did not persist");
      const auto loaded = files::loadSettings(fixture.paths());
      require(
          loaded && loaded->browser.gridIconSize == size && loaded->browser.showHidden, "zoom roundtrip lost settings"
      );
    }
    const auto previous = read(fixture.paths().overrides);
    for (const auto& value :
         {files::OverrideEdit{Preference::GRID_ICON_SIZE, true}, files::OverrideEdit{Preference::GRID_ICON_SIZE, 33}}) {
      const std::array edit{value};
      require(
          config.saveOverrides(edit) == State::NOT_COMMITTED && read(fixture.paths().overrides) == previous,
          "invalid zoom edit changed settings"
      );
    }
    const std::array reset{files::OverrideEdit{Preference::GRID_ICON_SIZE, std::monostate{}}};
    require(
        config.saveOverrides(reset) == State::DURABLE
            && config.settings().browser.gridIconSize == 64
            && !read(fixture.paths().overrides).contains("grid_icon_size"),
        "zoom reset did not restore base inheritance"
    );
  }

  void testSidebarWidth() {
    using Preference = files::BrowserPreference;
    using State = files::AtomicWriteState;
    Fixture fixture;
    files::ConfigService config(fixture.paths());
    require(config.reload() && config.settings().browser.sidebarWidth == 206, "sidebar default lost");
    fixture.write("config.toml", "[browser]\nsidebar_width = 280\nshow_hidden = true\n");
    fixture.write("settings.toml", "[browser]\nsidebar_width = 160\nview_mode = 'grid'\n");
    require(config.reload() && config.settings().browser.sidebarWidth == 160, "sidebar layering failed");
    for (const auto* invalid : {"159", "401", "true", "200.0", "'200'"}) {
      fixture.write("settings.toml", std::string("[browser]\nsidebar_width = ") + invalid + "\n");
      require(
          !config.reload() && config.settings().browser.sidebarWidth == 160, "invalid width replaced last-good settings"
      );
      require(config.issues().front().key == "browser.sidebar_width", "width diagnostic lost its key");
    }
    fixture.write("settings.toml", "[browser]\nview_mode = 'grid'\n");
    const std::array width{files::OverrideEdit{Preference::SIDEBAR_WIDTH, 400}};
    require(config.saveOverrides(width) == State::DURABLE, "sidebar override save failed");
    files::ConfigService reloaded(fixture.paths());
    require(
        reloaded.reload()
            && reloaded.settings().browser.sidebarWidth == 400
            && reloaded.settings().browser.viewMode == files::ViewMode::GRID
            && reloaded.settings().browser.showHidden,
        "sidebar roundtrip lost width or unrelated preferences"
    );
    const auto before = read(fixture.paths().overrides);
    for (const auto& value :
         {files::OverrideEdit{Preference::SIDEBAR_WIDTH, 159}, files::OverrideEdit{Preference::SIDEBAR_WIDTH, 401},
          files::OverrideEdit{Preference::SIDEBAR_WIDTH, true}}) {
      const std::array invalid{value};
      require(
          config.saveOverrides(invalid) == State::NOT_COMMITTED && read(fixture.paths().overrides) == before,
          "invalid width edit changed overrides"
      );
    }
    const std::array inherit{files::OverrideEdit{Preference::SIDEBAR_WIDTH, std::monostate{}}};
    require(
        config.saveOverrides(inherit) == State::DURABLE
            && config.settings().browser.sidebarWidth == 280
            && !read(fixture.paths().overrides).contains("sidebar_width"),
        "sidebar reset lost base inheritance"
    );
    require(
        read(fixture.paths().base) == "[browser]\nsidebar_width = 280\nshow_hidden = true\n",
        "sidebar save changed declarative base"
    );
  }

  void testSidebarVisibility() {
    using Preference = files::BrowserPreference;
    using State = files::AtomicWriteState;
    Fixture fixture;
    files::ConfigService config(fixture.paths());
    require(config.reload() && config.settings().browser.showSidebar, "sidebar should be visible by default");
    const std::string base = "[browser]\nshow_sidebar = false\nsidebar_width = 280\n";
    fixture.write("config.toml", base);
    fixture.write("settings.toml", "[browser]\nshow_sidebar = true\ngrid_icon_size = 64\n");
    require(config.reload() && config.settings().browser.showSidebar, "sidebar visibility layering failed");
    const std::array hide{files::OverrideEdit{Preference::SHOW_SIDEBAR, false}};
    require(config.saveOverrides(hide) == State::DURABLE, "hidden sidebar preference did not save");
    const auto loaded = files::loadSettings(fixture.paths());
    require(
        loaded
            && !loaded->browser.showSidebar
            && loaded->browser.sidebarWidth == 280
            && loaded->browser.gridIconSize == 64,
        "visibility roundtrip lost false or changed unrelated preferences"
    );
    const auto saved = read(fixture.paths().overrides);
    const std::array invalid{files::OverrideEdit{Preference::SHOW_SIDEBAR, 0}};
    require(
        config.saveOverrides(invalid) == State::NOT_COMMITTED && read(fixture.paths().overrides) == saved,
        "mistyped visibility edit changed settings"
    );
    const std::array show{files::OverrideEdit{Preference::SHOW_SIDEBAR, true}};
    require(
        config.saveOverrides(show) == State::DURABLE && config.settings().browser.showSidebar,
        "sidebar show did not save"
    );
    const std::array inherit{files::OverrideEdit{Preference::SHOW_SIDEBAR, std::monostate{}}};
    require(
        config.saveOverrides(inherit) == State::DURABLE
            && !config.settings().browser.showSidebar
            && !read(fixture.paths().overrides).contains("show_sidebar"),
        "visibility reset lost base inheritance"
    );
    require(read(fixture.paths().base) == base, "visibility save changed declarative base");
    for (const auto* value : {"0", "'false'", "1.0"}) {
      fixture.write("config.toml", std::string("[browser]\nshow_sidebar = ") + value + "\n");
      const auto issues = files::validateConfigFile(fixture.paths().base);
      require(
          issues.size() == 1 && issues.front().key == "browser.show_sidebar",
          "invalid visibility value accepted or misdiagnosed"
      );
    }
  }

  void testDetailsColumnVisibility() {
    using Preference = files::BrowserPreference;
    using State = files::AtomicWriteState;
    struct Column {
      const char* key;
      Preference preference;
      bool files::DetailsColumnVisibility::* member;
    };
    const std::array columns{
        Column{"show_type_column", Preference::SHOW_TYPE_COLUMN, &files::DetailsColumnVisibility::type},
        Column{"show_size_column", Preference::SHOW_SIZE_COLUMN, &files::DetailsColumnVisibility::size},
        Column{"show_modified_column", Preference::SHOW_MODIFIED_COLUMN, &files::DetailsColumnVisibility::modified},
    };
    for (const auto& column : columns) {
      Fixture fixture;
      files::ConfigService config(fixture.paths());
      require(
          config.reload() && config.settings().browser.columns == files::DetailsColumnVisibility{},
          "detail columns must be visible by default"
      );
      const auto base = std::string("[browser]\n") + column.key + " = false\nshow_hidden = true\n";
      fixture.write("config.toml", base);
      fixture.write("settings.toml", std::string("[browser]\n") + column.key + " = true\ngrid_icon_size = 64\n");
      require(
          config.reload() && config.settings().browser.columns.*column.member,
          "column override did not take precedence over base"
      );
      const std::array hide{files::OverrideEdit{column.preference, false}};
      require(config.saveOverrides(hide) == State::DURABLE, "column hide did not persist");
      const auto loaded = files::loadSettings(fixture.paths());
      require(
          loaded
              && !(loaded->browser.columns.*column.member)
              && loaded->browser.showHidden
              && loaded->browser.gridIconSize == 64,
          "column roundtrip lost false or unrelated preferences"
      );
      const auto saved = read(fixture.paths().overrides);
      const std::array invalid{files::OverrideEdit{column.preference, 0}};
      require(
          config.saveOverrides(invalid) == State::NOT_COMMITTED && read(fixture.paths().overrides) == saved,
          "mistyped column edit changed overrides"
      );
      const std::array inherit{files::OverrideEdit{column.preference, std::monostate{}}};
      require(
          config.saveOverrides(inherit) == State::DURABLE
              && !(config.settings().browser.columns.*column.member)
              && !read(fixture.paths().overrides).contains(column.key),
          "column reset did not restore base inheritance"
      );
      const std::array show{files::OverrideEdit{column.preference, true}};
      require(
          config.saveOverrides(show) == State::DURABLE && config.settings().browser.columns.*column.member,
          "column show did not persist"
      );
      require(read(fixture.paths().base) == base, "column save modified declarative base");
      for (const auto* value : {"0", "'false'", "[]"}) {
        fixture.write("config.toml", std::string("[browser]\n") + column.key + " = " + value + "\n");
        const auto issues = files::validateConfigFile(fixture.paths().base);
        require(
            issues.size() == 1 && issues.front().key == std::string("browser.") + column.key,
            "invalid column type accepted or misdiagnosed"
        );
      }
    }
  }

  void testDetailsColumnWidths() {
    using P = files::BrowserPreference;
    using S = files::AtomicWriteState;
    constexpr std::array preferences{
        P::NAME_COLUMN_WIDTH, P::TYPE_COLUMN_WIDTH, P::SIZE_COLUMN_WIDTH, P::MODIFIED_COLUMN_WIDTH
    };
    Fixture fixture;
    files::ConfigService config(fixture.paths());
    require(
        config.reload() && config.settings().browser.columnWidths == files::kDefaultDetailsColumnWidths,
        "default column allocations changed"
    );
    for (std::size_t index = 0; index < preferences.size(); ++index) {
      const std::string key(files::kDetailsColumnWidthKeys[index]);
      const std::string base = "[browser]\n" + key + " = 300\nshow_hidden = true\n";
      fixture.write("config.toml", base);
      fixture.write("settings.toml", "[browser]\ngrid_icon_size = 64\n");
      require(config.reload() && config.settings().browser.columnWidths[index] == 300, "base width did not load");
      for (const int width :
           {files::minimumDetailsColumnWidth(index), files::maximumDetailsColumnWidth(index),
            files::kDefaultDetailsColumnWidths[index]}) {
        const std::array edit{files::OverrideEdit{preferences[index], width}};
        require(config.saveOverrides(edit) == S::DURABLE, "valid column width failed to save");
        const auto loaded = files::loadSettings(fixture.paths());
        require(
            loaded
                && loaded->browser.columnWidths[index] == width
                && loaded->browser.showHidden
                && loaded->browser.gridIconSize == 64,
            "column width roundtrip lost values or unrelated settings"
        );
      }
      const auto saved = read(fixture.paths().overrides);
      for (const auto& bad : std::array{
               files::OverrideEdit{preferences[index], -1},
               files::OverrideEdit{preferences[index], files::maximumDetailsColumnWidth(index) + 1},
               files::OverrideEdit{preferences[index], true}
           })
        require(
            config.saveOverrides(std::span(&bad, 1)) == S::NOT_COMMITTED && read(fixture.paths().overrides) == saved,
            "invalid typed width mutated settings"
        );
      const std::array inherit{files::OverrideEdit{preferences[index], std::monostate{}}};
      require(
          config.saveOverrides(inherit) == S::DURABLE
              && config.settings().browser.columnWidths[index] == 300
              && !read(fixture.paths().overrides).contains(key)
              && read(fixture.paths().base) == base,
          "width inheritance reset failed or changed the base"
      );
      const auto lastGood = config.settings();
      for (const auto* invalid : {"-1", "1", "63", "16385", "9223372036854775807", "true", "160.0", "'160'", "[]"}) {
        fixture.write("config.toml", "[browser]\n" + key + " = " + invalid + "\n");
        const auto issues = files::validateConfigFile(fixture.paths().base);
        require(
            issues.size() == 1
                && issues.front().key == "browser." + key
                && !config.reload()
                && config.settings() == lastGood,
            "invalid width was accepted or replaced last-good settings"
        );
      }
      if (index != 0) {
        fixture.write("config.toml", "[browser]\n" + key + " = 0\n");
        require(!files::validateConfigFile(fixture.paths().base).empty(), "automatic metadata width was accepted");
      }
    }
  }

  void testDetailsColumnOrder() {
    using Column = files::DetailsColumn;
    using Preference = files::BrowserPreference;
    using State = files::AtomicWriteState;
    Fixture fixture;
    files::ConfigService config(fixture.paths());
    require(
        config.reload() && config.settings().browser.columnOrder == files::kDefaultDetailsColumnOrder,
        "default column order changed"
    );
    const files::DetailsColumnOrder baseOrder{Column::MODIFIED, Column::TYPE, Column::SIZE};
    const std::string base = "[browser]\ncolumn_order = ['modified', 'type', 'size']\nshow_size_column = false\n";
    fixture.write("config.toml", base);
    fixture.write("settings.toml", "[browser]\ngrid_icon_size = 64\n");
    require(config.reload() && config.settings().browser.columnOrder == baseOrder, "base column order did not load");
    auto order = files::kDefaultDetailsColumnOrder;
    do {
      const std::array edit{files::OverrideEdit{Preference::COLUMN_ORDER, order}};
      require(config.saveOverrides(edit) == State::DURABLE, "column permutation did not save");
      const auto loaded = files::loadSettings(fixture.paths());
      require(
          loaded
              && loaded->browser.columnOrder == order
              && !loaded->browser.columns.size
              && loaded->browser.gridIconSize == 64,
          "column permutation roundtrip lost order or unrelated preferences"
      );
    } while (std::ranges::next_permutation(order).found);
    const auto saved = read(fixture.paths().overrides);
    for (const auto& edit : std::array{
             files::OverrideEdit{Preference::COLUMN_ORDER, true},
             files::OverrideEdit{
                 Preference::COLUMN_ORDER, files::DetailsColumnOrder{Column::TYPE, Column::TYPE, Column::SIZE}
             },
             files::OverrideEdit{
                 Preference::COLUMN_ORDER,
                 files::DetailsColumnOrder{Column::TYPE, Column::SIZE, static_cast<Column>(99)}
             }
         }) {
      require(
          config.saveOverrides(std::span(&edit, 1)) == State::NOT_COMMITTED && read(fixture.paths().overrides) == saved,
          "invalid typed column order changed overrides"
      );
    }
    const std::array inherit{files::OverrideEdit{Preference::COLUMN_ORDER, std::monostate{}}};
    require(
        config.saveOverrides(inherit) == State::DURABLE
            && config.settings().browser.columnOrder == baseOrder
            && !read(fixture.paths().overrides).contains("column_order")
            && read(fixture.paths().base) == base,
        "column order reset changed base or failed to restore inheritance"
    );
    const auto lastGood = config.settings();
    for (const auto* invalid :
         {"true", "'type'", "[]", "['type', 'size']", "['type', 'size', 'size']", "['name', 'type', 'size']",
          "['Type', 'size', 'modified']", "['type', 'size', 1]", "['type', 'size', 'modified', 'name']"}) {
      fixture.write("config.toml", std::string("[browser]\ncolumn_order = ") + invalid + "\n");
      const auto issues = files::validateConfigFile(fixture.paths().base);
      require(
          issues.size() == 1
              && issues.front().key == "browser.column_order"
              && !config.reload()
              && config.settings() == lastGood,
          "invalid column array was accepted, misdiagnosed or replaced last-good settings"
      );
    }
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
    const std::array sort{files::OverrideEdit{Preference::SORT_BY, files::SortField::TYPE}};
    require(
        second.saveOverrides(sort) == State::DURABLE && second.settings().browser.sort.field == files::SortField::TYPE,
        "typed sort override failed"
    );
    const std::array grid{files::OverrideEdit{Preference::VIEW_MODE, files::ViewMode::GRID}};
    require(second.saveOverrides(grid) == State::DURABLE, "grid preference did not persist");
    files::ConfigService reloaded(fixture.paths());
    require(
        reloaded.reload()
            && reloaded.settings().browser.viewMode == files::ViewMode::GRID
            && reloaded.settings().browser.sort.field == files::SortField::TYPE,
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
    testGridIconSize();
    testSidebarWidth();
    testSidebarVisibility();
    testDetailsColumnVisibility();
    testDetailsColumnOrder();
    testDetailsColumnWidths();
    testOverrideSaving();
    std::println("settings checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "settings test: {}", error.what());
    return 1;
  }
}
