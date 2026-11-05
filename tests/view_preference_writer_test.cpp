#include "app/glib_poll_source.h"
#include "config/view_preference_writer.h"
#include "fs/gio_ptr.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>

namespace {
  void testSaving() {
    namespace fs = std::filesystem;
    const files::GString temporary(g_dir_make_tmp("noctalia-view-writer-XXXXXX", nullptr));
    TEST_CHECK(temporary != nullptr);
    const fs::path root(temporary.get());
    struct Cleanup {
      fs::path root;
      ~Cleanup() { fs::remove_all(root); }
    } cleanup{root};
    const files::ConfigPaths paths{root / "config.toml", root / "settings.toml"};
    std::ofstream(paths.base) << "[browser]\nshow_hidden = true\nview_mode = 'list'\n";
    GlibPollSource source;
    files::ViewPreferenceWriter writer(paths);
    std::string error;
    writer.setChangedCallback([&](const std::string& message) { error = message; });
    const auto wait = [&] {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (writer.busy()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = source.addPollFds(fds);
        const int timeout = source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), timeout < 0 ? 20 : std::min(timeout, 20)) >= 0);
        source.dispatch(fds, start);
      }
    };
    writer.save(files::ViewMode::GRID);
    writer.save(files::ViewMode::LIST);
    writer.save(files::ViewMode::GRID);
    writer.submit(
        {{files::BrowserPreference::ThumbnailPolicy, files::ThumbnailPolicy::NEVER},
         {files::BrowserPreference::ThumbnailCacheMib, 256}}
    );
    writer.submit(
        {{files::BrowserPreference::ThumbnailPolicy, files::ThumbnailPolicy::ALL},
         {files::BrowserPreference::ThumbnailCacheMib, 64}}
    );
    writer.saveGridIconSize(96);
    writer.saveGridIconSize(64);
    writer.saveSidebarWidth(320);
    writer.saveSidebarWidth(280);
    writer.saveShowSidebar(false);
    writer.saveShowSidebar(true);
    writer.saveShowSidebar(false);
    writer.saveShowHidden(false);
    writer.saveShowHidden(true);
    writer.saveShowHidden(false);
    writer.saveSort({files::SortField::NAME, true, true});
    writer.saveSort({files::SortField::SIZE, false, true});
    const files::SortOptions finalSort{files::SortField::TYPE, true, false};
    writer.saveSort(finalSort);
    writer.saveDetailsColumns({true, false, true});
    writer.saveDetailsColumns({false, true, false});
    const files::DetailsColumnVisibility hiddenColumns{false, false, false};
    writer.saveDetailsColumns(hiddenColumns);
    writer.saveColumnOrder({files::DetailsColumn::SIZE, files::DetailsColumn::TYPE, files::DetailsColumn::MODIFIED});
    const files::DetailsColumnOrder finalOrder{
        files::DetailsColumn::MODIFIED, files::DetailsColumn::SIZE, files::DetailsColumn::TYPE
    };
    writer.saveColumnOrder(finalOrder);
    writer.saveColumnWidths({400, 200, 100, 180});
    const files::DetailsColumnWidths finalWidths{640, 192, 96, 224};
    writer.saveColumnWidths(finalWidths);
    writer.saveInteraction(files::ActivationMode::SingleClick, files::TypingMode::SEARCH);
    writer.saveInteraction(files::ActivationMode::DoubleClick, files::TypingMode::TYPEAHEAD);
    writer.saveInteraction(files::ActivationMode::SingleClick, files::TypingMode::TYPEAHEAD);
    wait();
    const auto loaded = files::loadSettings(paths);
    TEST_CHECK(
        error.empty()
        && loaded
        && loaded->browser.viewMode == files::ViewMode::GRID
        && !loaded->browser.showHidden
        && loaded->browser.gridIconSize == 64
        && loaded->browser.thumbnails.policy == files::ThumbnailPolicy::ALL
        && loaded->browser.thumbnails.cacheMiB == 64
        && loaded->browser.sidebarWidth == 280
        && !loaded->browser.showSidebar
        && loaded->browser.sort == finalSort
        && loaded->browser.columns == hiddenColumns
        && loaded->browser.columnOrder == finalOrder
        && loaded->browser.columnWidths == finalWidths
        && loaded->browser.activationMode == files::ActivationMode::SingleClick
        && loaded->browser.typingMode == files::TypingMode::TYPEAHEAD
    );
    std::ifstream base(paths.base);
    const std::string content((std::istreambuf_iterator<char>(base)), {});
    TEST_CHECK(content == "[browser]\nshow_hidden = true\nview_mode = 'list'\n");
    writer.saveShowHidden(true);
    wait();
    const auto shown = files::loadSettings(paths);
    TEST_CHECK(
        error.empty()
        && shown
        && shown->browser.showHidden
        && shown->browser.gridIconSize == 64
        && !shown->browser.showSidebar
        && shown->browser.sort == finalSort
        && shown->browser.columns == hiddenColumns
        && shown->browser.columnOrder == finalOrder
        && shown->browser.columnWidths == finalWidths
        && shown->browser.activationMode == files::ActivationMode::SingleClick
        && shown->browser.typingMode == files::TypingMode::TYPEAHEAD
    );
    const files::SortOptions defaultSort;
    writer.saveSort(defaultSort);
    writer.saveDetailsColumns({});
    writer.saveColumnOrder(files::kDefaultDetailsColumnOrder);
    writer.saveColumnWidths(files::kDefaultDetailsColumnWidths);
    writer.saveInteraction(files::ActivationMode::DoubleClick, files::TypingMode::SEARCH);
    wait();
    const auto reset = files::loadSettings(paths);
    TEST_CHECK(
        error.empty()
        && reset
        && reset->browser.sort == defaultSort
        && reset->browser.showHidden
        && reset->browser.gridIconSize == 64
        && reset->browser.columns == files::DetailsColumnVisibility{}
        && reset->browser.columnOrder == files::kDefaultDetailsColumnOrder
        && reset->browser.columnWidths == files::kDefaultDetailsColumnWidths
        && reset->browser.activationMode == files::ActivationMode::DoubleClick
        && reset->browser.typingMode == files::TypingMode::SEARCH
    );
    std::ofstream(paths.overrides) << "[broken\n";
    writer.save(files::ViewMode::LIST);
    wait();
    TEST_CHECK(!error.empty());
    std::ifstream invalid(paths.overrides);
    TEST_CHECK(std::string((std::istreambuf_iterator<char>(invalid)), {}) == "[broken\n");
  }
  void testWatchingAndReset() {
    namespace fs = std::filesystem;
    using P = files::BrowserPreference;
    const files::GString temporary(g_dir_make_tmp("noctalia-settings-watch-XXXXXX", nullptr));
    TEST_CHECK(temporary != nullptr);
    const fs::path root(temporary.get());
    struct Cleanup {
      fs::path root;
      ~Cleanup() {
        std::error_code ignored;
        fs::permissions(root / "declarative.toml", fs::perms::owner_all, ignored);
        fs::remove_all(root, ignored);
      }
    } cleanup{root};
    const files::ConfigPaths paths{
        root / "config/noctalia-files/config.toml", root / "config/noctalia-files/settings.toml"
    };
    GlibPollSource source;
    auto writer = std::make_unique<files::ViewPreferenceWriter>(paths);
    std::string error;
    int changes = 0;
    writer->setChangedCallback([&](const std::string& message) {
      error = message;
      ++changes;
    });
    const auto until = [&](const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = source.addPollFds(fds);
        const int requested = source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) >= 0);
        source.dispatch(fds, start);
      }
    };
    const auto write = [&](const fs::path& path, const std::string& content) {
      std::ofstream(root / "staged.toml") << content;
      fs::rename(root / "staged.toml", path);
    };
    writer->startWatching();
    until([&] { return changes > 0 && !writer->busy(); });
    TEST_CHECK(error.empty() && writer->settings() == files::Settings{});
    fs::create_directories(paths.base.parent_path());
    const std::string base = "[browser]\nshow_hidden = true\nview_mode = 'grid'\nsidebar_width = 240\n";
    write(root / "declarative.toml", base);
    fs::create_symlink(root / "declarative.toml", paths.base);
    fs::permissions(root / "declarative.toml", fs::perms::owner_read);
    until([&] { return writer->settings()->browser.showHidden && !writer->busy(); });
    TEST_CHECK(writer->inheritedSettings()->browser.sidebarWidth == 240);
    writer->submit({{P::ShowHidden, false}, {P::ViewMode, files::ViewMode::LIST}});
    writer->saveGridIconSize(128);
    writer->reload();
    until([&] { return !writer->busy(); });
    TEST_CHECK(
        error.empty()
        && !writer->settings()->browser.showHidden
        && writer->settings()->browser.gridIconSize == 128
        && writer->settings()->browser.viewMode == files::ViewMode::LIST
        && writer->inheritedSettings()->browser.showHidden
    );
    writer->submit({{P::ShowHidden, std::monostate{}}});
    until([&] { return !writer->busy(); });
    TEST_CHECK(writer->settings()->browser.showHidden && writer->settings()->browser.gridIconSize == 128);
    const auto good = writer->settings();
    const auto previousChanges = changes;
    const std::string invalid = "[browser]\ngrid_icon_size = 45\n";
    write(paths.overrides, invalid);
    until([&] { return changes > previousChanges && !error.empty() && !writer->busy(); });
    TEST_CHECK(writer->settings() == good);
    writer->saveGridIconSize(64);
    until([&] { return !writer->busy(); });
    std::ifstream broken(paths.overrides);
    TEST_CHECK(std::string((std::istreambuf_iterator<char>(broken)), {}) == invalid);
    write(paths.overrides, "[browser]\ngrid_icon_size = 96\nshow_sidebar = false\n");
    until([&] { return writer->settings()->browser.gridIconSize == 96 && error.empty() && !writer->busy(); });
    TEST_CHECK(!writer->settings()->browser.showSidebar);
    fs::permissions(paths.overrides, fs::perms::owner_read);
    writer->saveShowSidebar(true);
    until([&] { return !writer->busy(); });
    TEST_CHECK(!error.empty() && !writer->settings()->browser.showSidebar);
    fs::permissions(paths.overrides, fs::perms::owner_read | fs::perms::owner_write);
    writer->resetAll();
    until([&] { return !writer->busy(); });
    TEST_CHECK(error.empty() && writer->settings() == writer->inheritedSettings());
    std::ifstream original(root / "declarative.toml");
    TEST_CHECK(std::string((std::istreambuf_iterator<char>(original)), {}) == base && fs::is_symlink(paths.base));
    write(root / "declarative.toml", "[browser]\nshow_hidden = false\nsidebar_width = 320\n");
    until([&] {
      return writer->settings()->browser.sidebarWidth == 320
          && !writer->settings()->browser.showHidden
          && !writer->busy();
    });
    fs::remove(paths.base);
    fs::remove(paths.overrides);
    until([&] { return writer->settings() == files::Settings{} && !writer->busy(); });
    write(paths.overrides, "[browser]\nview_mode = 'grid'\n");
    until([&] { return writer->settings()->browser.viewMode == files::ViewMode::GRID && !writer->busy(); });
    fs::rename(paths.base.parent_path(), root / "old-directory");
    until([&] { return writer->settings() == files::Settings{} && !writer->busy(); });
    fs::rename(root / "old-directory", paths.base.parent_path());
    until([&] { return writer->settings()->browser.viewMode == files::ViewMode::GRID && !writer->busy(); });
    write(paths.overrides, "[browser]\nview_mode = 'grid'\n");
    writer->reload();
    writer.reset();
    const auto drain = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    until([&] { return std::chrono::steady_clock::now() >= drain; });
    writer = std::make_unique<files::ViewPreferenceWriter>(paths);
    writer->setChangedCallback([&](const std::string&) { writer.reset(); });
    writer->startWatching();
    until([&] { return !writer; });
    const auto afterDestruction = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    until([&] { return std::chrono::steady_clock::now() >= afterDestruction; });
  }

} // namespace

int main() {
  try {
    testSaving();
    testWatchingAndReset();
    std::println("view preference writer checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "view preference writer test: {}", error.what());
    return 1;
  }
}
