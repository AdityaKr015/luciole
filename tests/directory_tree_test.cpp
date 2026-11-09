#include "browser/browser_controller.h"
#include "browser/directory_tree.h"
#include "config/settings.h"
#include "fs/gio_interop.h"
#include "test_check.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <thread>

namespace {
  namespace fs = std::filesystem;
  void wait(const std::function<bool()>& done) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!done()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(std::chrono::steady_clock::now() < end);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  void drain() {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    wait([&] { return std::chrono::steady_clock::now() >= end; });
  }
  files::EntryId find(const files::DirectoryModel& model, const fs::path& path) {
    const auto location = files::Location::fromPath(path.native());
    for (const auto id : model.rows()) {
      if (model.entry(id)->location == location) {
        return id;
      }
    }
    return 0;
  }
  auto ready(files::BrowserController& browser) {
    wait([&] { return browser.model().snapshot().status != files::DirectoryStatus::Loading; });
    TEST_CHECK(browser.model().snapshot().status == files::DirectoryStatus::Ready);
  }
  void details(const fs::path& root) {
    fs::create_directories(root / "a" / "nested");
    fs::create_directories(root / "b");
    std::ofstream(root / "a" / "nested" / "café-<literal>-文.txt") << "payload";
    std::ofstream(root / "a" / "z.txt") << "z";
    std::ofstream(root / "a" / ".hidden") << 'h';
    fs::create_directory_symlink("..", root / "a" / "cycle");
    files::BrowserController browser({}, false);
    browser.open(files::Location::fromPath(root.native()));
    ready(browser);
    TEST_CHECK(!browser.expandableFolders() && browser.model().rows().size() == 2);
    browser.setExpandableFolders(true);
    auto& model = browser.model();
    const auto a = find(model, root / "a");
    TEST_CHECK(browser.toggleFolder(a));
    wait([&] { return !browser.treeRow(a).loading; });
    TEST_CHECK(model.snapshot().location == files::Location::fromPath(root.native()));
    const auto nested = find(model, root / "a" / "nested");
    TEST_CHECK(nested && model.treeParent(nested) == a && model.treeDepth(nested) == 1);
    TEST_CHECK(!find(model, root / "a" / ".hidden"));
    TEST_CHECK(!browser.treeRow(find(model, root / "a" / "cycle")).expandable);
    TEST_CHECK(browser.toggleFolder(nested));
    wait([&] { return !browser.treeRow(nested).loading; });
    const auto leaf = find(model, root / "a" / "nested" / "café-<literal>-文.txt");
    TEST_CHECK(leaf && model.treeDepth(leaf) == 2);
    TEST_CHECK(model.select(leaf));
    std::optional<files::Location> opened;
    browser.setFileActivatedCallback([&](const auto& location) { opened = location; });
    browser.activate(leaf);
    TEST_CHECK(opened == files::Location::fromPath((root / "a" / "nested" / "café-<literal>-文.txt").native()));
    auto sort = model.sort();
    sort.descending = true;
    model.setSort(sort);
    TEST_CHECK(model.selection().selected.contains(leaf));
    const auto position = std::ranges::find(model.rows(), nested);
    TEST_CHECK(position != model.rows().end() && *(position + 1) == leaf);
    browser.refresh();
    ready(browser);
    wait([&] { return !browser.treeRow(a).loading && !browser.treeRow(nested).loading; });
    TEST_CHECK(find(model, root / "a" / "nested" / "café-<literal>-文.txt") == leaf);
    TEST_CHECK(model.selection().selected.contains(leaf));
    browser.toggleHidden();
    TEST_CHECK(find(model, root / "a" / ".hidden"));
    TEST_CHECK(browser.toggleFolder(a));
    TEST_CHECK(!model.entry(leaf) && !model.selection().selected.contains(leaf));
    TEST_CHECK(browser.toggleFolder(a));
    wait([&] { return !browser.treeRow(a).loading; });
    TEST_CHECK(!browser.treeRow(find(model, root / "a" / "nested")).expanded);
    model.select(a);
    TEST_CHECK(browser.treeKey(true));
    TEST_CHECK(model.treeParent(*model.selection().focused) == a);
    TEST_CHECK(browser.treeKey(false) && model.selection().focused == a);
    browser.setSearchQuery("z.txt");
    ready(browser);
    TEST_CHECK(!browser.treeRow(a).enabled && model.rows().size() == 1);
    browser.setSearchQuery({});
    ready(browser);
    TEST_CHECK(!browser.treeRow(a).expanded);
    browser.setExpandableFolders(false);
    TEST_CHECK(!browser.treeRow(a).enabled && model.rows().size() == 2);
  }
  files::DirectoryEntry rootEntry(const fs::path& path, files::EntryId id) {
    return {
        .id = id,
        .location = files::Location::fromPath(path.native()),
        .name = path.filename().native(),
        .displayName = path.filename().native(),
        .type = files::FileType::Directory,
        .size = {},
        .modifiedSeconds = {},
        .etag = {},
        .parentDisplayName = {}
    };
  }
  void boundsAndSidebar(const fs::path& root) {
    files::DirectoryModel model;
    files::DirectoryTree tree(model, true);
    files::DirectorySnapshot snapshot;
    snapshot.status = files::DirectoryStatus::Ready;
    snapshot.entries = {rootEntry(root / "b", 10), rootEntry(root / "a", 20), rootEntry(root / "a" / "nested", 30)};
    tree.setRoot(snapshot);
    TEST_CHECK((model.rows() == std::vector<files::EntryId>{10, 20, 30}));
    TEST_CHECK(tree.expand(20));
    wait([&] { return !tree.row(20).loading; });
    TEST_CHECK(model.rows().size() == 4); // Only real subfolders, no file/symlink entries.
    const auto child = model.rows()[2];
    TEST_CHECK(model.treeParent(child) == 20 && model.entry(child)->location == model.entry(30)->location);
    TEST_CHECK(tree.expand(child));
    wait([&] { return !tree.row(child).loading; });
    TEST_CHECK(tree.row(child).expanded && !tree.row(30).expanded); // Same URI, independent tree occurrences.
    TEST_CHECK(tree.collapse(20) && tree.branches() == 0);
    snapshot.entries.clear();
    for (std::size_t i = 0; i < files::DirectoryTree::maxBranches + 1; ++i) {
      const auto path = root / ("branch-" + std::to_string(i));
      fs::create_directory(path);
      snapshot.entries.push_back(rootEntry(path, i + 1));
    }
    tree.setRoot(snapshot);
    for (std::size_t i = 0; i < files::DirectoryTree::maxBranches; ++i) {
      TEST_CHECK(tree.expand(i + 1));
      TEST_CHECK(tree.loading() <= files::DirectoryTree::maxLoading);
    }
    TEST_CHECK(!tree.expand(files::DirectoryTree::maxBranches + 1));
    TEST_CHECK(
        tree.branches() == files::DirectoryTree::maxBranches
        && !tree.row(files::DirectoryTree::maxBranches + 1).status.empty()
    );
    tree.refresh();
    TEST_CHECK(tree.loading() <= files::DirectoryTree::maxLoading);
    wait([&] { return tree.loading() == 0; });
    tree.clear();
    TEST_CHECK(tree.branches() == 0);
    drain();
    auto cycleRoot = rootEntry(root / "a", 1);
    const auto info = files::GioPtr<GFileInfo>(g_file_query_info(
        files::gioFile(files::Location::fromPath((root / "a" / "nested").native())).get(), "id::file,id::filesystem",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    cycleRoot.identity = files::readFileIdentity(info.get());
    TEST_CHECK(cycleRoot.identity);
    snapshot.entries = {cycleRoot};
    tree.setRoot(snapshot);
    TEST_CHECK(tree.expand(1));
    wait([&] { return !tree.row(1).loading; });
    const auto repeated = find(model, root / "a" / "nested");
    TEST_CHECK(repeated && !tree.expand(repeated) && tree.row(repeated).status == "Folder cycle blocked");
    snapshot.entries.front().identity = files::FileIdentity{"changed", "changed"};
    tree.setRoot(snapshot);
    TEST_CHECK(tree.branches() == 0 && model.rows().size() == 1);
    const auto deep = root / "deep";
    auto path = deep;
    for (std::size_t depth = 0; depth <= files::DirectoryTree::maxDepth + 1; ++depth) {
      path /= "level";
      fs::create_directories(path);
    }
    snapshot.entries = {rootEntry(deep, 1)};
    tree.setRoot(snapshot);
    auto current = files::EntryId{1};
    path = deep;
    for (std::size_t depth = 0; depth < files::DirectoryTree::maxDepth; ++depth) {
      TEST_CHECK(tree.expand(current));
      wait([&] { return !tree.row(current).loading; });
      path /= "level";
      current = find(model, path);
      TEST_CHECK(current);
    }
    TEST_CHECK(model.treeDepth(current) == files::DirectoryTree::maxDepth && !tree.expand(current));
    tree.clear();
    const auto many = root / "many";
    fs::create_directory(many);
    for (std::size_t i = 0; i < files::DirectoryTree::maxChildren + 20; ++i) {
      fs::create_directory(many / std::to_string(i));
    }
    snapshot.entries = {rootEntry(many, 1)};
    tree.setRoot(snapshot);
    TEST_CHECK(tree.expand(1));
    wait([&] { return !tree.row(1).loading; });
    TEST_CHECK(model.rows().size() == files::DirectoryTree::maxChildren + 1 && !tree.row(1).status.empty());
    TEST_CHECK(tree.collapse(1) && model.rows().size() == 1);
    auto missing = rootEntry(root / "missing", 2);
    snapshot.entries = {missing};
    tree.setRoot(snapshot);
    TEST_CHECK(tree.expand(2));
    wait([&] { return !tree.row(2).loading; });
    TEST_CHECK(!tree.row(2).status.empty());
    tree.clear();
    snapshot.entries = {rootEntry(root / "a", 1)};
    tree.setRoot(snapshot);
    TEST_CHECK(tree.expand(1));
    tree.clear();
    drain();
    TEST_CHECK(tree.branches() == 0);
    {
      files::DirectoryModel temporary;
      files::DirectoryTree retiring(temporary);
      retiring.setRoot(snapshot);
      TEST_CHECK(retiring.expand(1));
    }
    drain();
  }
  void settings(const fs::path& root) {
    using P = files::BrowserPreference;
    const files::ConfigPaths paths{root / "config.toml", root / "settings.toml"};
    files::ConfigService service(paths);
    TEST_CHECK(service.reload());
    TEST_CHECK(!service.settings().browser.sidebarTree && !service.settings().browser.expandableFolders);
    std::ofstream(paths.base) << "[browser]\nsidebar_tree = true\nexpandable_folders = true\nshow_hidden = true\n";
    TEST_CHECK(service.reload());
    const std::array edits{
        files::OverrideEdit{P::SidebarTree, false}, files::OverrideEdit{P::ExpandableFolders, false}
    };
    TEST_CHECK(service.saveOverrides(edits) == files::AtomicWriteState::Durable);
    files::ConfigService restored(paths);
    TEST_CHECK(restored.reload());
    TEST_CHECK(
        !restored.settings().browser.sidebarTree
        && !restored.settings().browser.expandableFolders
        && restored.settings().browser.showHidden
    );
    const std::array reset{
        files::OverrideEdit{P::SidebarTree, std::nullopt}, files::OverrideEdit{P::ExpandableFolders, std::nullopt}
    };
    TEST_CHECK(restored.saveOverrides(reset) == files::AtomicWriteState::Durable);
    TEST_CHECK(restored.settings().browser.sidebarTree && restored.settings().browser.expandableFolders);
    for (const auto* key : {"sidebar_tree", "expandable_folders"}) {
      std::ofstream(paths.base) << "[browser]\n" << key << " = 'true'\n";
      TEST_CHECK(!files::validateConfigFile(paths.base).empty());
      TEST_CHECK(!restored.reload() && restored.settings().browser.sidebarTree);
    }
  }
} // namespace
int main() {
  const files::GString temporary(g_dir_make_tmp("noctalia-directory-tree-XXXXXX", nullptr));
  TEST_CHECK(temporary);
  const fs::path root(temporary.get());
  details(root);
  boundsAndSidebar(root);
  settings(root);
  fs::remove_all(root);
  std::println("directory tree checks passed");
}
