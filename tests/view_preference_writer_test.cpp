#include "app/glib_poll_source.h"
#include "config/view_preference_writer.h"
#include "fs/gio_ptr.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>

namespace {
  void require(bool condition, const char* message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }
  void testSaving() {
    namespace fs = std::filesystem;
    const files::GString temporary(g_dir_make_tmp("noctalia-view-writer-XXXXXX", nullptr));
    require(temporary != nullptr, "cannot create settings fixture");
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
        require(std::chrono::steady_clock::now() < deadline, "settings writer timed out");
        std::vector<pollfd> fds;
        const auto start = source.addPollFds(fds);
        const int timeout = source.pollTimeoutMs();
        require(::poll(fds.data(), fds.size(), timeout < 0 ? 20 : std::min(timeout, 20)) >= 0, "poll failed");
        source.dispatch(fds, start);
      }
    };
    writer.save(files::ViewMode::GRID);
    writer.save(files::ViewMode::LIST);
    writer.save(files::ViewMode::GRID);
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
    wait();
    const auto loaded = files::loadSettings(paths);
    require(
        error.empty()
            && loaded
            && loaded->browser.viewMode == files::ViewMode::GRID
            && !loaded->browser.showHidden
            && loaded->browser.gridIconSize == 64
            && loaded->browser.sidebarWidth == 280
            && !loaded->browser.showSidebar
            && loaded->browser.sort == finalSort
            && loaded->browser.columns == hiddenColumns,
        "rapid changes did not persist the latest view or overwrote unrelated preferences"
    );
    std::ifstream base(paths.base);
    const std::string content((std::istreambuf_iterator<char>(base)), {});
    require(content == "[browser]\nshow_hidden = true\nview_mode = 'list'\n", "writer changed the declarative base");
    writer.saveShowHidden(true);
    wait();
    const auto shown = files::loadSettings(paths);
    require(
        error.empty()
            && shown
            && shown->browser.showHidden
            && shown->browser.gridIconSize == 64
            && !shown->browser.showSidebar
            && shown->browser.sort == finalSort
            && shown->browser.columns == hiddenColumns,
        "showing hidden entries did not persist or lost unrelated overrides"
    );
    const files::SortOptions defaultSort;
    writer.saveSort(defaultSort);
    writer.saveDetailsColumns({});
    wait();
    const auto reset = files::loadSettings(paths);
    require(
        error.empty()
            && reset
            && reset->browser.sort == defaultSort
            && reset->browser.showHidden
            && reset->browser.gridIconSize == 64
            && reset->browser.columns == files::DetailsColumnVisibility{},
        "restoring default sort choices did not persist or lost unrelated overrides"
    );
    std::ofstream(paths.overrides) << "[broken\n";
    writer.save(files::ViewMode::LIST);
    wait();
    require(!error.empty(), "invalid sidecar save failure was hidden");
    std::ifstream invalid(paths.overrides);
    require(
        std::string((std::istreambuf_iterator<char>(invalid)), {}) == "[broken\n",
        "failed save replaced invalid settings"
    );
  }
} // namespace

int main() {
  try {
    testSaving();
    std::println("view preference writer checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "view preference writer test: {}", error.what());
    return 1;
  }
}
