#include "browser/directory_model.h"
#include "config/settings.h"
#include "test_check.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <print>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  struct Fixture {
    std::filesystem::path directory;
    Fixture() {
      const files::GString name(g_dir_make_tmp("noctalia-settings-test-XXXXXX", nullptr));
      TEST_CHECK(name != nullptr);
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
      TEST_CHECK(file.good());
    }
    files::ConfigPaths paths() const {
      return {.base = directory / "config.toml", .overrides = directory / "settings.toml"};
    }
  };

  std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }

  void testMillerSettings() {
    Fixture fixture;
    fixture.write("config.toml", "[browser]\nview_mode = 'miller'\nmiller_column_width = 300\n");
    files::ConfigService settings(fixture.paths());
    TEST_CHECK(
        settings.reload()
        && settings.settings().browser.viewMode == files::ViewMode::Miller
        && settings.settings().browser.millerColumnWidth == 300
    );
    using P = files::BrowserPreference;
    const std::array edits{
        files::OverrideEdit{P::ViewMode, files::ViewMode::Miller}, files::OverrideEdit{P::MillerColumnWidth, 417}
    };
    TEST_CHECK(settings.saveOverrides(edits) == files::AtomicWriteState::Durable);
    files::ConfigService restored(fixture.paths());
    TEST_CHECK(
        restored.reload()
        && restored.settings().browser.millerColumnWidth == 417
        && restored.settings().browser.viewMode == files::ViewMode::Miller
    );
    const auto before = read(fixture.paths().overrides);
    for (const int width : {179, 801}) {
      const std::array invalid{files::OverrideEdit{P::MillerColumnWidth, width}};
      TEST_CHECK(
          restored.saveOverrides(invalid) == files::AtomicWriteState::NotCommitted
          && read(fixture.paths().overrides) == before
      );
      fixture.write("config.toml", std::format("[browser]\nmiller_column_width = {}\n", width));
      TEST_CHECK(!files::validateConfigFile(fixture.paths().base).empty());
    }
    fixture.write("config.toml", "[browser]\nview_mode = 'miller'\nmiller_column_width = 300\n");
    const std::array reset{files::OverrideEdit{P::MillerColumnWidth, std::nullopt}};
    TEST_CHECK(
        restored.saveOverrides(reset) == files::AtomicWriteState::Durable
        && restored.settings().browser.millerColumnWidth == 300
    );
  }

  void testMillerParents() {
    Fixture fixture;
    files::ConfigService settings(fixture.paths());
    TEST_CHECK(settings.reload() && settings.settings().browser.millerShowParents);
    using P = files::BrowserPreference;
    const std::array off{files::OverrideEdit{P::MillerShowParents, false}};
    TEST_CHECK(settings.saveOverrides(off) == files::AtomicWriteState::Durable);
    files::ConfigService restored(fixture.paths());
    TEST_CHECK(restored.reload() && !restored.settings().browser.millerShowParents);
    fixture.write("config.toml", "[browser]\nmiller_show_parents = false\n");
    const std::array on{files::OverrideEdit{P::MillerShowParents, true}};
    TEST_CHECK(
        restored.saveOverrides(on) == files::AtomicWriteState::Durable && restored.settings().browser.millerShowParents
    );
    const std::array reset{files::OverrideEdit{P::MillerShowParents, std::nullopt}};
    TEST_CHECK(
        restored.saveOverrides(reset) == files::AtomicWriteState::Durable
        && !restored.settings().browser.millerShowParents
    );
    fixture.write("config.toml", "[browser]\nmiller_show_parents = 'true'\n");
    TEST_CHECK(!files::validateConfigFile(fixture.paths().base).empty());
  }

  void testSchemaAndDiagnostics() {
    Fixture fixture;
    fixture.write(
        "config.toml",
        "schema_version = 1\n[browser]\nshow_hidden = true\nsort_by = 'modified'\n"
        "sort_descending = true\ndirectories_first = false\nview_mode = 'grid'\n"
    );
    TEST_CHECK(files::validateConfigFile(fixture.paths().base).empty());
    fixture.write("settings.toml", "invalid adjacent override");
    TEST_CHECK(files::validateConfigFile(fixture.paths().base).empty());
    fixture.write("config.toml", "unexpected = 1\n[browser]\nshow_hidden = 1\nsort_by = 'date'\nunknown = false\n");
    const auto errors = files::validateConfigFile(fixture.paths().base);
    TEST_CHECK(errors.size() == 4);
    bool foundLine = false;
    for (const auto& error : errors) {
      TEST_CHECK(error.file == fixture.paths().base && error.line > 0 && error.column > 0);
      if (error.key == "browser.show_hidden") {
        foundLine = error.line == 3 && error.format().contains("config.toml:3:") && error.message.contains("boolean");
      }
    }
    TEST_CHECK(foundLine);
    for (const auto& invalid :
         {"schema_version = 2", "schema_version = true", "schema_version = 1.0", "browser = false",
          "[browser]\nshow_hidden = 'true'", "[browser]\nsort_by = 1", "[browser]\nview_mode = true",
          "[browser]\nview_mode = 'icons'", "[browser]\nshow_hidden = true\nshow_hidden = false", "broken = [",
          "theme = {}"}) {
      fixture.write("config.toml", invalid);
      TEST_CHECK(!files::validateConfigFile(fixture.paths().base).empty());
    }
    fixture.write("config.toml", "");
    TEST_CHECK(files::validateConfigFile(fixture.paths().base).empty());
    fixture.write("config.toml", "[browser]\nsort_by = 'type'\n");
    std::filesystem::remove(fixture.paths().overrides);
    const auto typed = files::loadSettings(fixture.paths());
    TEST_CHECK(typed && typed->browser.sort.field == files::SortField::Type);
  }

  void testLayersAndReload() {
    Fixture fixture;
    files::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings() == files::Settings{});
    fixture.write(
        "config.toml", "[browser]\nshow_hidden = true\nsort_by = 'size'\nsort_descending = true\nview_mode = 'grid'\n"
    );
    TEST_CHECK(config.reload() && config.settings().browser.viewMode == files::ViewMode::Grid);
    fixture.write("settings.toml", "[browser]\nshow_hidden = false\ndirectories_first = false\nview_mode = 'list'\n");
    TEST_CHECK(config.reload());
    const auto& browser = config.settings().browser;
    TEST_CHECK(
        !browser.showHidden
        && browser.sort.field == files::SortField::Size
        && browser.sort.descending
        && !browser.sort.directoriesFirst
        && browser.viewMode == files::ViewMode::List
    );
    const auto lastGood = config.settings();
    fixture.write("settings.toml", "[browser]\nview_mode = 'unknown'\n");
    TEST_CHECK(!config.reload() && config.settings() == lastGood && !config.issues().empty());
    fixture.write("config.toml", "[browser]\nshow_hidden = 'not-a-boolean'\n");
    fixture.write("settings.toml", "[browser]\nshow_hidden = true\n");
    TEST_CHECK(!config.reload() && config.settings() == lastGood);
    std::filesystem::remove(fixture.paths().base);
    std::filesystem::remove(fixture.paths().overrides);
    TEST_CHECK(config.reload() && config.issues().empty() && config.settings() == files::Settings{});
  }

  void testFilesAndLimits() {
    Fixture fixture;
    TEST_CHECK(!files::validateConfigFile(fixture.paths().base).empty());
    fixture.write("target.toml", "[browser]\nshow_hidden = true\n");
    const auto target = fixture.directory / "target.toml";
    std::filesystem::create_symlink(target, fixture.paths().base);
    TEST_CHECK(::chmod(target.c_str(), 0444) == 0);
    files::ConfigService config(fixture.paths());
    const auto original = read(target);
    TEST_CHECK(config.reload() && config.settings().browser.showHidden);
    TEST_CHECK(read(target) == original && std::filesystem::is_symlink(fixture.paths().base));
    std::filesystem::remove(fixture.paths().base);
    std::filesystem::create_symlink(fixture.directory / "missing-target", fixture.paths().base);
    TEST_CHECK(!config.reload());
    std::filesystem::remove(fixture.paths().base);
    std::filesystem::create_directory(fixture.paths().base);
    TEST_CHECK(!config.reload());
    std::filesystem::remove(fixture.paths().base);
    TEST_CHECK(::mkfifo(fixture.paths().base.c_str(), 0600) == 0);
    TEST_CHECK(!config.reload());
    std::filesystem::remove(fixture.paths().base);
    std::string bounded(256 * 1024, '#');
    fixture.write("config.toml", bounded);
    TEST_CHECK(config.reload());
    bounded.push_back('#');
    fixture.write("config.toml", bounded);
    TEST_CHECK(!config.reload() && config.issues().front().message.contains("256 KiB"));
    if (::geteuid() != 0) {
      fixture.write("config.toml", "");
      TEST_CHECK(::chmod(fixture.paths().base.c_str(), 0000) == 0);
      const bool loaded = config.reload();
      ::chmod(fixture.paths().base.c_str(), 0600);
      TEST_CHECK(!loaded);
    }
  }

  void testEnvironmentPaths() {
    const char* previous = std::getenv("XDG_CONFIG_HOME");
    const std::optional<std::string> saved = previous != nullptr ? std::optional<std::string>(previous) : std::nullopt;
    ::setenv("XDG_CONFIG_HOME", "/tmp/noctalia-settings-path-test", 1);
    const auto paths = files::ConfigPaths::fromEnvironment();
    TEST_CHECK(
        paths.base == "/tmp/noctalia-settings-path-test/noctalia-files/config.toml"
        && paths.overrides == "/tmp/noctalia-settings-path-test/noctalia-files/settings.toml"
    );
    ::setenv("XDG_CONFIG_HOME", "relative-invalid-path", 1);
    bool rejected = false;
    try {
      (void)files::ConfigPaths::fromEnvironment();
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    TEST_CHECK(rejected);
    ::unsetenv("XDG_CONFIG_HOME");
    const auto fallback = files::ConfigPaths::fromEnvironment();
    TEST_CHECK(fallback.base == std::filesystem::path(std::getenv("HOME")) / ".config/noctalia-files/config.toml");
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
    TEST_CHECK(loaded.has_value());
    files::DirectoryModel model;
    model.setSort(loaded->browser.sort);
    model.setFilter({.showHidden = loaded->browser.showHidden, .text = {}});
    files::DirectorySnapshot data;
    data.location = files::Location::fromPath(fixture.directory.native());
    data.generation = 1;
    data.status = files::DirectoryStatus::Ready;
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
    TEST_CHECK((model.rows() == std::vector<files::EntryId>{1, 2}));
  }

  void testGridIconSize() {
    Fixture fixture;
    using Preference = files::BrowserPreference;
    using State = files::AtomicWriteState;
    for (const int size : files::kGridIconSizes) {
      fixture.write("config.toml", "[browser]\ngrid_icon_size = " + std::to_string(size) + "\n");
      const auto loaded = files::loadSettings(fixture.paths());
      TEST_CHECK(loaded && loaded->browser.gridIconSize == size);
    }
    for (const auto* value : {"0", "31", "49", "129", "257", "-32", "64.0", "true", "'64'", "9223372036854775807"}) {
      fixture.write("config.toml", std::string("[browser]\ngrid_icon_size = ") + value + "\n");
      TEST_CHECK(!files::validateConfigFile(fixture.paths().base).empty());
    }
    fixture.write("config.toml", "[browser]\ngrid_icon_size = 64\n");
    fixture.write("settings.toml", "[browser]\ngrid_icon_size = 96\nshow_hidden = true\n");
    files::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings().browser.gridIconSize == 96);
    for (const int size : {32, 256}) {
      const std::array zoom{files::OverrideEdit{Preference::GridIconSize, size}};
      TEST_CHECK(config.saveOverrides(zoom) == State::Durable);
      const auto loaded = files::loadSettings(fixture.paths());
      TEST_CHECK(loaded && loaded->browser.gridIconSize == size && loaded->browser.showHidden);
    }
    const auto previous = read(fixture.paths().overrides);
    const std::array invalid{files::OverrideEdit{Preference::GridIconSize, 33}};
    TEST_CHECK(config.saveOverrides(invalid) == State::NotCommitted && read(fixture.paths().overrides) == previous);
    const std::array reset{files::OverrideEdit{Preference::GridIconSize, std::nullopt}};
    TEST_CHECK(
        config.saveOverrides(reset) == State::Durable
        && config.settings().browser.gridIconSize == 64
        && !read(fixture.paths().overrides).contains("grid_icon_size")
    );
  }

  void testSidebarWidth() {
    using Preference = files::BrowserPreference;
    using State = files::AtomicWriteState;
    Fixture fixture;
    files::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings().browser.sidebarWidth == 206);
    fixture.write("config.toml", "[browser]\nsidebar_width = 280\nshow_hidden = true\n");
    fixture.write("settings.toml", "[browser]\nsidebar_width = 160\nview_mode = 'grid'\n");
    TEST_CHECK(config.reload() && config.settings().browser.sidebarWidth == 160);
    for (const auto* invalid : {"159", "401", "true", "200.0", "'200'"}) {
      fixture.write("settings.toml", std::string("[browser]\nsidebar_width = ") + invalid + "\n");
      TEST_CHECK(!config.reload() && config.settings().browser.sidebarWidth == 160);
      TEST_CHECK(config.issues().front().key == "browser.sidebar_width");
    }
    fixture.write("settings.toml", "[browser]\nview_mode = 'grid'\n");
    const std::array width{files::OverrideEdit{Preference::SidebarWidth, 400}};
    TEST_CHECK(config.saveOverrides(width) == State::Durable);
    files::ConfigService reloaded(fixture.paths());
    TEST_CHECK(
        reloaded.reload()
        && reloaded.settings().browser.sidebarWidth == 400
        && reloaded.settings().browser.viewMode == files::ViewMode::Grid
        && reloaded.settings().browser.showHidden
    );
    const auto before = read(fixture.paths().overrides);
    for (const auto& value :
         {files::OverrideEdit{Preference::SidebarWidth, 159}, files::OverrideEdit{Preference::SidebarWidth, 401}}) {
      const std::array invalid{value};
      TEST_CHECK(config.saveOverrides(invalid) == State::NotCommitted && read(fixture.paths().overrides) == before);
    }
    const std::array inherit{files::OverrideEdit{Preference::SidebarWidth, std::nullopt}};
    TEST_CHECK(
        config.saveOverrides(inherit) == State::Durable
        && config.settings().browser.sidebarWidth == 280
        && !read(fixture.paths().overrides).contains("sidebar_width")
    );
    TEST_CHECK(read(fixture.paths().base) == "[browser]\nsidebar_width = 280\nshow_hidden = true\n");
  }

  void testPreviewVisibility() {
    using P = files::BrowserPreference;
    Fixture fixture;
    files::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings().browser.showPreview);
    fixture.write("config.toml", "[browser]\nshow_preview = true\nshow_hidden = true\n");
    TEST_CHECK(config.reload() && config.settings().browser.showPreview);
    TEST_CHECK(
        config.saveOverrides(std::array{files::OverrideEdit{P::ShowPreview, false}}) == files::AtomicWriteState::Durable
    );
    const auto loaded = files::loadSettings(fixture.paths());
    TEST_CHECK(loaded && !loaded->browser.showPreview && loaded->browser.showHidden);
    TEST_CHECK(
        config.saveOverrides(std::array{files::OverrideEdit{P::ShowPreview, std::nullopt}})
            == files::AtomicWriteState::Durable
        && config.settings().browser.showPreview
    );
    fixture.write("config.toml", "[browser]\nshow_preview = 'true'\n");
    TEST_CHECK(!files::validateConfigFile(fixture.paths().base).empty());
    fixture.write("config.toml", "[browser]\npreview_width = 444\nshow_preview = true\n");
    TEST_CHECK(config.reload() && config.settings().browser.previewWidth == 444);
    TEST_CHECK(
        config.saveOverrides(std::array{files::OverrideEdit{P::PreviewWidth, 555}}) == files::AtomicWriteState::Durable
    );
    TEST_CHECK(files::loadSettings(fixture.paths())->browser.previewWidth == 555);
    const auto before = read(fixture.paths().overrides);
    for (int width : {207, 801}) {
      TEST_CHECK(
          config.saveOverrides(std::array{files::OverrideEdit{P::PreviewWidth, width}})
              == files::AtomicWriteState::NotCommitted
          && read(fixture.paths().overrides) == before
      );
    }
    TEST_CHECK(
        config.saveOverrides(std::array{files::OverrideEdit{P::PreviewWidth, std::nullopt}})
            == files::AtomicWriteState::Durable
        && config.settings().browser.previewWidth == 444
    );
    for (const auto* value : {"207", "801", "true", "'320'", "320.0"}) {
      fixture.write("config.toml", std::string("[browser]\npreview_width = ") + value + "\n");
      TEST_CHECK(!files::validateConfigFile(fixture.paths().base).empty());
    }
  }
  void testSidebarVisibility() {
    using Preference = files::BrowserPreference;
    using State = files::AtomicWriteState;
    Fixture fixture;
    files::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings().browser.showSidebar);
    const std::string base = "[browser]\nshow_sidebar = false\nsidebar_width = 280\n";
    fixture.write("config.toml", base);
    fixture.write("settings.toml", "[browser]\nshow_sidebar = true\ngrid_icon_size = 64\n");
    TEST_CHECK(config.reload() && config.settings().browser.showSidebar);
    const std::array hide{files::OverrideEdit{Preference::ShowSidebar, false}};
    TEST_CHECK(config.saveOverrides(hide) == State::Durable);
    const auto loaded = files::loadSettings(fixture.paths());
    TEST_CHECK(
        loaded
        && !loaded->browser.showSidebar
        && loaded->browser.sidebarWidth == 280
        && loaded->browser.gridIconSize == 64
    );
    const std::array show{files::OverrideEdit{Preference::ShowSidebar, true}};
    TEST_CHECK(config.saveOverrides(show) == State::Durable && config.settings().browser.showSidebar);
    const std::array inherit{files::OverrideEdit{Preference::ShowSidebar, std::nullopt}};
    TEST_CHECK(
        config.saveOverrides(inherit) == State::Durable
        && !config.settings().browser.showSidebar
        && !read(fixture.paths().overrides).contains("show_sidebar")
    );
    TEST_CHECK(read(fixture.paths().base) == base);
    for (const auto* value : {"0", "'false'", "1.0"}) {
      fixture.write("config.toml", std::string("[browser]\nshow_sidebar = ") + value + "\n");
      const auto issues = files::validateConfigFile(fixture.paths().base);
      TEST_CHECK(issues.size() == 1 && issues.front().key == "browser.show_sidebar");
    }
  }

  void testDetailsColumnVisibility() {
    using Preference = files::BrowserPreference;
    using State = files::AtomicWriteState;
    struct Column {
      const char* key;
      files::PreferenceKey<bool> preference;
      bool files::DetailsColumnVisibility::* member;
    };
    const std::array columns{
        Column{"show_type_column", Preference::ShowTypeColumn, &files::DetailsColumnVisibility::type},
        Column{"show_size_column", Preference::ShowSizeColumn, &files::DetailsColumnVisibility::size},
        Column{"show_modified_column", Preference::ShowModifiedColumn, &files::DetailsColumnVisibility::modified},
    };
    for (const auto& column : columns) {
      Fixture fixture;
      files::ConfigService config(fixture.paths());
      TEST_CHECK(config.reload() && config.settings().browser.columns == files::DetailsColumnVisibility{});
      const auto base = std::string("[browser]\n") + column.key + " = false\nshow_hidden = true\n";
      fixture.write("config.toml", base);
      fixture.write("settings.toml", std::string("[browser]\n") + column.key + " = true\ngrid_icon_size = 64\n");
      TEST_CHECK(config.reload() && config.settings().browser.columns.*column.member);
      const std::array hide{files::OverrideEdit{column.preference, false}};
      TEST_CHECK(config.saveOverrides(hide) == State::Durable);
      const auto loaded = files::loadSettings(fixture.paths());
      TEST_CHECK(
          loaded
          && !(loaded->browser.columns.*column.member)
          && loaded->browser.showHidden
          && loaded->browser.gridIconSize == 64
      );
      const std::array inherit{files::OverrideEdit{column.preference, std::nullopt}};
      TEST_CHECK(
          config.saveOverrides(inherit) == State::Durable
          && !(config.settings().browser.columns.*column.member)
          && !read(fixture.paths().overrides).contains(column.key)
      );
      const std::array show{files::OverrideEdit{column.preference, true}};
      TEST_CHECK(config.saveOverrides(show) == State::Durable && config.settings().browser.columns.*column.member);
      TEST_CHECK(read(fixture.paths().base) == base);
      for (const auto* value : {"0", "'false'", "[]"}) {
        fixture.write("config.toml", std::string("[browser]\n") + column.key + " = " + value + "\n");
        const auto issues = files::validateConfigFile(fixture.paths().base);
        TEST_CHECK(issues.size() == 1 && issues.front().key == std::string("browser.") + column.key);
      }
    }
  }

  void testInteractionModes() {
    using A = files::ActivationMode;
    using T = files::TypingMode;
    using P = files::BrowserPreference;
    using S = files::AtomicWriteState;
    Fixture fixture;
    files::ConfigService config(fixture.paths());
    TEST_CHECK(
        config.reload()
        && config.settings().browser.activationMode == A::DoubleClick
        && config.settings().browser.typingMode == T::Search
    );
    const std::string base =
        "[browser]\nactivation_mode = 'single_click'\ntyping_mode = 'typeahead'\nshow_hidden = true\n";
    fixture.write("config.toml", base);
    fixture.write("settings.toml", "[browser]\ngrid_icon_size = 96\n");
    for (const auto activation : {A::SingleClick, A::DoubleClick}) {
      for (const auto typing : {T::Search, T::Typeahead}) {
        const std::array edits{
            files::OverrideEdit{P::ActivationMode, activation}, files::OverrideEdit{P::TypingMode, typing}
        };
        TEST_CHECK(config.saveOverrides(edits) == S::Durable);
        const auto loaded = files::loadSettings(fixture.paths());
        TEST_CHECK(
            loaded
            && loaded->browser.activationMode == activation
            && loaded->browser.typingMode == typing
            && loaded->browser.showHidden
            && loaded->browser.gridIconSize == 96
        );
      }
    }
    const auto saved = read(fixture.paths().overrides);
    for (const auto& bad : std::array{
             files::OverrideEdit{P::ActivationMode, static_cast<A>(99)},
             files::OverrideEdit{P::TypingMode, static_cast<T>(99)}
         }) {
      TEST_CHECK(
          config.saveOverrides(std::span(&bad, 1)) == S::NotCommitted && read(fixture.paths().overrides) == saved
      );
    }
    const std::array inherit{
        files::OverrideEdit{P::ActivationMode, std::nullopt}, files::OverrideEdit{P::TypingMode, std::nullopt}
    };
    TEST_CHECK(
        config.saveOverrides(inherit) == S::Durable
        && config.settings().browser.activationMode == A::SingleClick
        && config.settings().browser.typingMode == T::Typeahead
        && read(fixture.paths().base) == base
    );
    const auto lastGood = config.settings();
    for (const auto* key : {"activation_mode", "typing_mode"}) {
      for (const auto* bad : {"true", "1", "[]", "'single'", "'Search'", "'double-click'"}) {
        fixture.write("config.toml", std::string("[browser]\n") + key + " = " + bad + "\n");
        const auto issues = files::validateConfigFile(fixture.paths().base);
        TEST_CHECK(
            issues.size() == 1
            && issues.front().key == std::string("browser.") + key
            && !config.reload()
            && config.settings() == lastGood
        );
      }
    }
  }

  void testDetailsColumnWidths() {
    using P = files::BrowserPreference;
    using S = files::AtomicWriteState;
    constexpr std::array preferences{
        P::NameColumnWidth, P::TypeColumnWidth, P::SizeColumnWidth, P::ModifiedColumnWidth
    };
    Fixture fixture;
    files::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings().browser.columnWidths == files::kDefaultDetailsColumnWidths);
    for (std::size_t index = 0; index < preferences.size(); ++index) {
      const std::string key(files::kDetailsColumnWidthKeys[index]);
      const std::string base = "[browser]\n" + key + " = 300\nshow_hidden = true\n";
      fixture.write("config.toml", base);
      fixture.write("settings.toml", "[browser]\ngrid_icon_size = 64\n");
      TEST_CHECK(config.reload() && config.settings().browser.columnWidths[index] == 300);
      for (const int width :
           {files::minimumDetailsColumnWidth(index), files::maximumDetailsColumnWidth(index),
            files::kDefaultDetailsColumnWidths[index]}) {
        const std::array edit{files::OverrideEdit{preferences[index], width}};
        TEST_CHECK(config.saveOverrides(edit) == S::Durable);
        const auto loaded = files::loadSettings(fixture.paths());
        TEST_CHECK(
            loaded
            && loaded->browser.columnWidths[index] == width
            && loaded->browser.showHidden
            && loaded->browser.gridIconSize == 64
        );
      }
      const auto saved = read(fixture.paths().overrides);
      for (const auto& bad : std::array{
               files::OverrideEdit{preferences[index], -1},
               files::OverrideEdit{preferences[index], files::maximumDetailsColumnWidth(index) + 1}
           }) {
        TEST_CHECK(
            config.saveOverrides(std::span(&bad, 1)) == S::NotCommitted && read(fixture.paths().overrides) == saved
        );
      }
      const std::array inherit{files::OverrideEdit{preferences[index], std::nullopt}};
      TEST_CHECK(
          config.saveOverrides(inherit) == S::Durable
          && config.settings().browser.columnWidths[index] == 300
          && !read(fixture.paths().overrides).contains(key)
          && read(fixture.paths().base) == base
      );
      const auto lastGood = config.settings();
      for (const auto* invalid : {"-1", "1", "63", "16385", "9223372036854775807", "true", "160.0", "'160'", "[]"}) {
        fixture.write("config.toml", "[browser]\n" + key + " = " + invalid + "\n");
        const auto issues = files::validateConfigFile(fixture.paths().base);
        TEST_CHECK(
            issues.size() == 1
            && issues.front().key == "browser." + key
            && !config.reload()
            && config.settings() == lastGood
        );
      }
      if (index != 0) {
        fixture.write("config.toml", "[browser]\n" + key + " = 0\n");
        TEST_CHECK(!files::validateConfigFile(fixture.paths().base).empty());
      }
    }
  }

  void testDetailsColumnOrder() {
    using Column = files::DetailsColumn;
    using Preference = files::BrowserPreference;
    using State = files::AtomicWriteState;
    Fixture fixture;
    files::ConfigService config(fixture.paths());
    TEST_CHECK(config.reload() && config.settings().browser.columnOrder == files::kDefaultDetailsColumnOrder);
    const files::DetailsColumnOrder baseOrder{Column::Modified, Column::Type, Column::Size};
    const std::string base = "[browser]\ncolumn_order = ['modified', 'type', 'size']\nshow_size_column = false\n";
    fixture.write("config.toml", base);
    fixture.write("settings.toml", "[browser]\ngrid_icon_size = 64\n");
    TEST_CHECK(config.reload() && config.settings().browser.columnOrder == baseOrder);
    auto order = files::kDefaultDetailsColumnOrder;
    do {
      const std::array edit{files::OverrideEdit{Preference::ColumnOrder, order}};
      TEST_CHECK(config.saveOverrides(edit) == State::Durable);
      const auto loaded = files::loadSettings(fixture.paths());
      TEST_CHECK(
          loaded
          && loaded->browser.columnOrder == order
          && !loaded->browser.columns.size
          && loaded->browser.gridIconSize == 64
      );
    } while (std::ranges::next_permutation(order).found);
    const auto saved = read(fixture.paths().overrides);
    for (const auto& edit : std::array{
             files::OverrideEdit{
                 Preference::ColumnOrder, files::DetailsColumnOrder{Column::Type, Column::Type, Column::Size}
             },
             files::OverrideEdit{
                 Preference::ColumnOrder, files::DetailsColumnOrder{Column::Type, Column::Size, static_cast<Column>(99)}
             }
         }) {
      TEST_CHECK(
          config.saveOverrides(std::span(&edit, 1)) == State::NotCommitted && read(fixture.paths().overrides) == saved
      );
    }
    const std::array inherit{files::OverrideEdit{Preference::ColumnOrder, std::nullopt}};
    TEST_CHECK(
        config.saveOverrides(inherit) == State::Durable
        && config.settings().browser.columnOrder == baseOrder
        && !read(fixture.paths().overrides).contains("column_order")
        && read(fixture.paths().base) == base
    );
    const auto lastGood = config.settings();
    for (const auto* invalid :
         {"true", "'type'", "[]", "['type', 'size']", "['type', 'size', 'size']", "['name', 'type', 'size']",
          "['Type', 'size', 'modified']", "['type', 'size', 1]", "['type', 'size', 'modified', 'name']"}) {
      fixture.write("config.toml", std::string("[browser]\ncolumn_order = ") + invalid + "\n");
      const auto issues = files::validateConfigFile(fixture.paths().base);
      TEST_CHECK(
          issues.size() == 1
          && issues.front().key == "browser.column_order"
          && !config.reload()
          && config.settings() == lastGood
      );
    }
  }

  void testThumbnailSettings() {
    using P = files::BrowserPreference;
    Fixture fixture;
    fixture.write(
        "config.toml",
        "[browser]\nthumbnail_policy = 'all'\nthumbnail_max_file_mib = 128\nthumbnail_max_megapixels = "
        "24\nthumbnail_cache_mib = 256\n"
    );
    files::ConfigService config(fixture.paths());
    TEST_CHECK(
        (config.reload()
         && config.settings().browser.thumbnails == files::ThumbnailOptions{files::ThumbnailPolicy::All, 128, 24, 256})
    );
    const std::array edits{
        files::OverrideEdit{P::ThumbnailPolicy, files::ThumbnailPolicy::Never},
        files::OverrideEdit{P::ThumbnailMaxFileMib, 512}, files::OverrideEdit{P::ThumbnailMaxMegapixels, 48},
        files::OverrideEdit{P::ThumbnailCacheMib, 512}
    };
    TEST_CHECK(config.saveOverrides(edits) == files::AtomicWriteState::Durable);
    const auto loaded = files::loadSettings(fixture.paths());
    TEST_CHECK(
        (loaded && loaded->browser.thumbnails == files::ThumbnailOptions{files::ThumbnailPolicy::Never, 512, 48, 512})
    );
    const std::array reset{
        files::OverrideEdit{P::ThumbnailPolicy, std::nullopt},
        files::OverrideEdit{P::ThumbnailMaxFileMib, std::nullopt},
        files::OverrideEdit{P::ThumbnailMaxMegapixels, std::nullopt},
        files::OverrideEdit{P::ThumbnailCacheMib, std::nullopt}
    };
    TEST_CHECK(
        (config.saveOverrides(reset) == files::AtomicWriteState::Durable
         && config.settings().browser.thumbnails == files::ThumbnailOptions{files::ThumbnailPolicy::All, 128, 24, 256})
    );
    for (const auto* invalid :
         {"thumbnail_policy = true", "thumbnail_policy = 'remote'", "thumbnail_max_file_mib = 0",
          "thumbnail_max_file_mib = 1025", "thumbnail_max_megapixels = 0", "thumbnail_max_megapixels = 65",
          "thumbnail_cache_mib = 15", "thumbnail_cache_mib = 1025", "thumbnail_max_file_mib = 1.5"}) {
      fixture.write("config.toml", std::string("[browser]\n") + invalid + "\n");
      TEST_CHECK(!files::validateConfigFile(fixture.paths().base).empty());
    }
    fixture.write("config.toml", "");
    for (const auto preference : {P::ThumbnailMaxFileMib, P::ThumbnailMaxMegapixels, P::ThumbnailCacheMib}) {
      const std::array invalid{files::OverrideEdit{preference, -1}};
      TEST_CHECK(config.saveOverrides(invalid) == files::AtomicWriteState::NotCommitted);
    }
  }

  void testOverrideSaving() {
    using Preference = files::BrowserPreference;
    using State = files::AtomicWriteState;
    Fixture fixture;
    fixture.write("base-target.toml", "[browser]\nshow_hidden = false\nsort_by = 'size'\n");
    const auto baseTarget = fixture.directory / "base-target.toml";
    std::filesystem::create_symlink(baseTarget, fixture.paths().base);
    TEST_CHECK(::chmod(baseTarget.c_str(), 0444) == 0);
    const auto baseContent = read(baseTarget);
    files::ConfigService first(fixture.paths());
    files::ConfigService second(fixture.paths());
    TEST_CHECK(first.reload() && second.reload());
    const std::array show{files::OverrideEdit{Preference::ShowHidden, true}};
    const std::array descending{files::OverrideEdit{Preference::SortDescending, true}};
    TEST_CHECK(first.saveOverrides(show) == State::Durable && first.settings().browser.showHidden);
    // The second instance still has its earlier cache. Saving another key must
    // reread the sidecar under the lock rather than overwrite the first edit.
    TEST_CHECK(second.saveOverrides(descending) == State::Durable);
    TEST_CHECK(
        second.settings().browser.showHidden
        && second.settings().browser.sort.descending
        && second.settings().browser.sort.field == files::SortField::Size
    );
    TEST_CHECK(files::validateConfigFile(fixture.paths().overrides).empty());
    const auto saved = read(fixture.paths().overrides);
    TEST_CHECK(second.saveOverrides(descending) == State::Unchanged && read(fixture.paths().overrides) == saved);
    const std::array disable{
        files::OverrideEdit{Preference::DirectoriesFirst, false},
        files::OverrideEdit{Preference::SortDescending, false},
    };
    TEST_CHECK(
        second.saveOverrides(disable) == State::Durable
        && second.settings().browser.showHidden
        && !second.settings().browser.sort.directoriesFirst
        && !second.settings().browser.sort.descending
    );
    const std::array inherit{files::OverrideEdit{Preference::ShowHidden, std::nullopt}};
    TEST_CHECK(second.saveOverrides(inherit) == State::Durable && !second.settings().browser.showHidden);
    TEST_CHECK(!read(fixture.paths().overrides).contains("show_hidden"));
    TEST_CHECK(read(baseTarget) == baseContent && std::filesystem::is_symlink(fixture.paths().base));

    const auto lastGood = second.settings();
    fixture.write("settings.toml", "[browser]\nunknown = true\n");
    const auto invalid = read(fixture.paths().overrides);
    TEST_CHECK(
        second.saveOverrides(show) == State::NotCommitted
        && second.settings() == lastGood
        && read(fixture.paths().overrides) == invalid
        && !second.issues().empty()
    );
    std::filesystem::remove(fixture.paths().overrides);
    std::filesystem::create_symlink(baseTarget, fixture.paths().overrides);
    TEST_CHECK(second.saveOverrides(show) == State::NotCommitted && read(baseTarget) == baseContent);
    std::filesystem::remove(fixture.paths().overrides);
    const std::array invalidEdits{files::OverrideEdit{Preference::SortBy, static_cast<files::SortField>(99)}};
    TEST_CHECK(
        second.saveOverrides(invalidEdits) == State::NotCommitted && !std::filesystem::exists(fixture.paths().overrides)
    );
    const std::array sort{files::OverrideEdit{Preference::SortBy, files::SortField::Type}};
    TEST_CHECK(
        second.saveOverrides(sort) == State::Durable && second.settings().browser.sort.field == files::SortField::Type
    );
    const std::array grid{files::OverrideEdit{Preference::ViewMode, files::ViewMode::Grid}};
    TEST_CHECK(second.saveOverrides(grid) == State::Durable);
    files::ConfigService reloaded(fixture.paths());
    TEST_CHECK(
        reloaded.reload()
        && reloaded.settings().browser.viewMode == files::ViewMode::Grid
        && reloaded.settings().browser.sort.field == files::SortField::Type
    );
    const auto gridBytes = read(fixture.paths().overrides);
    const std::array invalidMode{files::OverrideEdit{Preference::ViewMode, static_cast<files::ViewMode>(99)}};
    TEST_CHECK(
        second.saveOverrides(invalidMode) == State::NotCommitted && read(fixture.paths().overrides) == gridBytes
    );
    const std::array resetView{files::OverrideEdit{Preference::ViewMode, std::nullopt}};
    TEST_CHECK(
        second.saveOverrides(resetView) == State::Durable
        && second.settings().browser.viewMode == files::ViewMode::List
        && !read(fixture.paths().overrides).contains("view_mode")
    );
    files::ConfigService aliased({.base = fixture.paths().base, .overrides = fixture.paths().base});
    TEST_CHECK(aliased.saveOverrides(show) == State::NotCommitted && read(baseTarget) == baseContent);
    Fixture reverseAlias;
    reverseAlias.write("settings.toml", "[browser]\nshow_hidden = false\n");
    const auto aliasedContent = read(reverseAlias.paths().overrides);
    std::filesystem::create_symlink(reverseAlias.paths().overrides, reverseAlias.paths().base);
    files::ConfigService sameFile(reverseAlias.paths());
    TEST_CHECK(
        sameFile.saveOverrides(show) == State::NotCommitted && read(reverseAlias.paths().overrides) == aliasedContent
    );

    Fixture invalidBase;
    invalidBase.write("config.toml", "[browser]\nshow_hidden = true\n");
    files::ConfigService protectedConfig(invalidBase.paths());
    TEST_CHECK(protectedConfig.saveOverrides(descending) == State::Durable);
    const auto before = read(invalidBase.paths().overrides);
    const auto beforeSettings = protectedConfig.settings();
    invalidBase.write("config.toml", "schema_version = 999\n");
    TEST_CHECK(
        protectedConfig.saveOverrides(sort) == State::NotCommitted
        && protectedConfig.settings() == beforeSettings
        && read(invalidBase.paths().overrides) == before
    );
  }
} // namespace

int main() {
  try {
    testSchemaAndDiagnostics();
    testMillerSettings();
    testMillerParents();
    testLayersAndReload();
    testFilesAndLimits();
    testEnvironmentPaths();
    testBrowserDefaults();
    testGridIconSize();
    testSidebarWidth();
    testSidebarVisibility();
    testPreviewVisibility();
    testDetailsColumnVisibility();
    testDetailsColumnOrder();
    testDetailsColumnWidths();
    testInteractionModes();
    testThumbnailSettings();
    testOverrideSaving();
    std::println("settings checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "settings test: {}", error.what());
    return 1;
  }
}
