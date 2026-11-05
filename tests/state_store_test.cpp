#include "config/state_store.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <print>
#include <stdexcept>
#include <sys/stat.h>

namespace {
  namespace fs = std::filesystem;
  using files::ActivePane;
  using files::AtomicWriteState;
  using files::Bookmark;
  using files::Location;
  using files::SavedWindow;
  using files::StateStore;

  void require(bool condition, const std::string& message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  struct Fixture {
    fs::path directory;
    Fixture() {
      const files::GString name(g_dir_make_tmp("noctalia-state-test-XXXXXX", nullptr));
      require(name != nullptr, "cannot create private state fixture");
      directory = name.get();
    }
    ~Fixture() {
      std::error_code ignored;
      fs::remove_all(directory, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    fs::path path() const { return directory / "state.toml"; }
  };

  void write(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
    require(output.good(), "cannot write state fixture");
  }

  std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  SavedWindow window(const std::string& path) {
    return {
        .tabs = {{.primary = Location::fromPath(path), .secondary = std::nullopt, .activePane = ActivePane::PRIMARY}},
        .activeTab = 0
    };
  }

  void testRoundtrip() {
    Fixture fixture;
    const auto path = fixture.directory / "not-created" / "state.toml";
    StateStore store(path);
    require(store.saveBookmarks({}) == AtomicWriteState::NotCommitted, "save was allowed before loading");
    require(store.load() && !fs::exists(path.parent_path()), "missing state was created during load");
    const std::string native =
        (fixture.directory / "space #percent%\n").native() + std::string(1, static_cast<char>(0xFF));
    const auto local = Location::fromPath(native);
    const auto remote = Location::fromUri("sftp://alice@example.test/shared%20folder");
    std::string deepNative = fixture.directory.native();
    for (int i = 0; i < 100; ++i) {
      deepNative += '/' + std::string(30, static_cast<char>(0xFF));
    }
    const auto deepLocation = Location::fromPath(deepNative);
    require(deepLocation.uri().size() > 8192, "fixture did not exercise expanded native URI length");
    const std::vector<Bookmark> bookmarks{
        {.label = "Work \"files\"", .location = local},
        {.label = "共享", .location = remote},
        {.label = "Deep native path", .location = deepLocation}
    };
    require(store.saveBookmarks(bookmarks) == AtomicWriteState::DURABLE, "bookmark save failed");
    std::vector<SavedWindow> windows{window("/tmp/second-tab"), window("/tmp/other-window")};
    windows[0].tabs.insert(
        windows[0].tabs.begin(), {.primary = local, .secondary = remote, .activePane = ActivePane::SECONDARY}
    );
    windows[0].activeTab = 1;
    require(store.saveWindows(windows) == AtomicWriteState::DURABLE, "session snapshot save failed");
    StateStore restored(path);
    require(
        restored.load() && restored.state().bookmarks == bookmarks && restored.state().windows == windows,
        "structured state roundtrip changed order, active tab or pane"
    );
    require(
        restored.state().bookmarks.front().location.nativePath() == native, "native URI bytes changed in persistence"
    );
    require(read(path).contains("schema_version = 1"), "state has no explicit version");
    const auto bytes = read(path);
    struct stat before{};
    require(::stat(path.c_str(), &before) == 0 && (before.st_mode & 0777) == 0600, "state is not private");
    require(
        restored.saveWindows(windows) == AtomicWriteState::UNCHANGED && read(path) == bytes,
        "idempotent save rewrote state"
    );
    struct stat after{};
    require(::stat(path.c_str(), &after) == 0 && before.st_ino == after.st_ino, "no-op save replaced the state inode");
  }

  void testSectionMergingAndConflicts() {
    Fixture fixture;
    StateStore first(fixture.path());
    StateStore second(fixture.path());
    require(first.load() && second.load(), "cannot load section fixtures");
    const std::vector<Bookmark> a{{.label = "A", .location = Location::fromPath("/tmp/a")}};
    const std::vector<Bookmark> b{{.label = "B", .location = Location::fromPath("/tmp/b")}};
    const std::vector<Bookmark> c{{.label = "C", .location = Location::fromPath("/tmp/c")}};
    require(first.saveBookmarks(a) == AtomicWriteState::DURABLE, "first bookmark save failed");
    require(
        second.saveWindows({window("/tmp/session-one")}) == AtomicWriteState::DURABLE && second.state().bookmarks == a,
        "stale window save lost bookmarks"
    );
    require(
        first.saveBookmarks(b) == AtomicWriteState::DURABLE && first.state().windows == second.state().windows,
        "bookmark update lost another instance's window snapshot"
    );
    const auto before = read(fixture.path());
    const auto lastGood = second.state();
    require(
        second.saveBookmarks(c) == AtomicWriteState::NotCommitted
            && read(fixture.path()) == before
            && second.state() == lastGood
            && !second.issues().empty(),
        "conflicting bookmark list was overwritten"
    );
    require(second.load() && second.saveBookmarks(c) == AtomicWriteState::DURABLE, "reloaded bookmark retry failed");
    require(
        first.saveWindows({window("/tmp/session-two")}) == AtomicWriteState::DURABLE && first.state().bookmarks == c,
        "window save failed to preserve newer bookmarks"
    );
    const auto currentBytes = read(fixture.path());
    require(
        second.saveWindows({window("/tmp/stale-session")}) == AtomicWriteState::NotCommitted
            && read(fixture.path()) == currentBytes,
        "conflicting session snapshot was overwritten"
    );
    require(
        second.saveBookmarks(c) == AtomicWriteState::UNCHANGED && second.state() == first.state(),
        "no-op save did not refresh the other section"
    );
  }

  void testEmptyPlacesSnapshot() {
    Fixture fixture;
    StateStore first(fixture.path());
    require(first.load() && !first.state().placesCustomized, "missing state already customized Places");
    require(first.saveWindows({window("/tmp/initial")}) == AtomicWriteState::DURABLE, "cannot save initial window");
    StateStore stale(fixture.path());
    require(stale.load() && !stale.state().placesCustomized, "window-only save initialized Places");
    require(first.saveBookmarks({}) == AtomicWriteState::DURABLE, "empty Places was treated as an unsaved no-op");
    require(
        stale.saveBookmarks({{.label = "Stale", .location = Location::fromPath("/tmp/stale")}})
            == AtomicWriteState::NotCommitted,
        "stale defaults overwrote an explicitly empty Places list"
    );
    require(
        stale.saveWindows({window("/tmp/updated")}) == AtomicWriteState::DURABLE,
        "window save failed to preserve customized Places"
    );
    require(
        first.load() && first.state().placesCustomized && first.state().bookmarks.empty(),
        "empty Places did not survive unrelated window saving"
    );
    const auto bytes = read(fixture.path());
    require(
        first.saveBookmarks({}) == AtomicWriteState::UNCHANGED && read(fixture.path()) == bytes,
        "an already saved empty Places list was rewritten"
    );
  }

  void testInvalidStateAndRecovery() {
    Fixture fixture;
    StateStore store(fixture.path());
    require(
        store.load() && store.saveWindows({window("/tmp/valid")}) == AtomicWriteState::DURABLE,
        "cannot create valid state"
    );
    const auto lastGood = store.state();
    for (const auto& invalid : {
             "",
             "schema_version = 2",
             "schema_version = true",
             "broken = [",
             "schema_version = 1\nunknown = true",
             "schema_version = 1\nbookmarks = 0",
             "schema_version = 1\nplaces_customized = 1",
             "schema_version = 1\nplaces_customized = true",
             "schema_version = 1\nbookmarks = [{label = 'x', uri = '/relative'}]",
             "schema_version = 1\nbookmarks = [{label = '', uri = 'file:///tmp'}]",
             "schema_version = 1\nbookmarks = [{label = 'x', uri = 'file:///tmp', extra = true}]",
             "schema_version = 1\nwindows = [{active_tab = 0, tabs = []}]",
             "schema_version = 1\nwindows = [{active_tab = -1, tabs = []}]",
             "schema_version = 1\nwindows = [{active_tab = 2, tabs = [{primary_uri = 'file:///tmp', active_pane = "
             "'primary'}]}]",
             "schema_version = 1\nwindows = [{active_tab = 0, tabs = [{primary_uri = 'file:///tmp', active_pane = "
             "'secondary'}]}]",
             "schema_version = 1\nwindows = [{active_tab = 0, tabs = [{primary_uri = 'file:///tmp', active_pane = "
             "'invalid'}]}]",
         }) {
      write(fixture.path(), invalid);
      require(
          !store.load() && store.state() == lastGood && !store.issues().empty(),
          "invalid state replaced last-good snapshot"
      );
      require(
          store.saveWindows({window("/tmp/replacement")}) == AtomicWriteState::NotCommitted
              && read(fixture.path()) == invalid,
          "save silently overwrote corrupt/future state"
      );
    }
    write(fixture.path(), "schema_version = true\n");
    require(
        !store.load() && store.issues().front().line == 1 && store.issues().front().key == "schema_version",
        "state version diagnostic lost its source"
    );
    fs::remove(fixture.path());
    require(store.load() && store.state() == files::SessionState{}, "explicit removal did not reset state");
  }

  void testPrivacyAndBounds() {
    Fixture fixture;
    StateStore store(fixture.path());
    require(
        store.load() && store.saveWindows({window("/tmp/valid")}) == AtomicWriteState::DURABLE,
        "cannot create privacy fixture"
    );
    const auto original = read(fixture.path());
    for (const std::string uri :
         {"sftp://alice:super-secret@example.test/path", "sftp://alice;token=super-secret@example.test/path",
          "https://example.test/path?token=super-secret", "https://example.test/path#super-secret"}) {
      const std::vector<Bookmark> bookmarks{{.label = "Remote", .location = Location::fromUri(uri)}};
      require(
          store.saveBookmarks(bookmarks) == AtomicWriteState::NotCommitted && read(fixture.path()) == original,
          "sensitive URI was persisted"
      );
      for (const auto& issue : store.issues()) {
        require(!issue.format().contains("super-secret"), "credential leaked into diagnostic");
      }
      write(fixture.path(), "schema_version = 1\nbookmarks = [{label = 'Remote', uri = '" + uri + "'}]\n");
      require(!store.load(), "sensitive URI was accepted from disk");
      write(fixture.path(), original);
    }
    const auto local = Location::fromPath("/tmp/repeated");
    require(
        store.saveBookmarks({{.label = "one", .location = local}, {.label = "two", .location = local}})
            == AtomicWriteState::NotCommitted,
        "duplicate bookmark locations accepted"
    );
    for (const auto& label :
         {std::string(257, 'x'), std::string("line\nbreak"), std::string(1, static_cast<char>(0xFF))}) {
      require(
          store.saveBookmarks({{.label = label, .location = local}}) == AtomicWriteState::NotCommitted,
          "invalid bookmark label accepted"
      );
    }
    require(
        store.saveWindows(std::vector<SavedWindow>(17, window("/tmp/window"))) == AtomicWriteState::NotCommitted,
        "window count limit ignored"
    );
    auto manyTabs = window("/tmp/tabs");
    manyTabs.tabs.resize(65, manyTabs.tabs.front());
    require(store.saveWindows({manyTabs}) == AtomicWriteState::NotCommitted, "tab count limit ignored");
    require(read(fixture.path()) == original, "rejected typed state modified the file");
  }

  void testPrivateReaderAndPaths() {
    Fixture fixture;
    StateStore store(fixture.path());
    require(
        store.load() && store.saveWindows({window("/tmp/valid")}) == AtomicWriteState::DURABLE,
        "cannot create private reader fixture"
    );
    const auto original = read(fixture.path());
    require(::chmod(fixture.path().c_str(), 0444) == 0 && store.load(), "read-only private state could not load");
    require(
        store.saveBookmarks({{.label = "x", .location = Location::fromPath("/tmp/x")}})
            == AtomicWriteState::NotCommitted,
        "save replaced read-only state"
    );
    struct stat info{};
    require(
        ::stat(fixture.path().c_str(), &info) == 0 && (info.st_mode & 0777) == 0444, "load changed state permissions"
    );
    const auto target = fixture.directory / "target";
    fs::rename(fixture.path(), target);
    fs::create_symlink(target, fixture.path());
    require(!store.load() && read(target) == original, "state load followed a symlink");
    fs::remove(fixture.path());
    fs::create_hard_link(target, fixture.path());
    require(!store.load(), "state load followed a hard link");
    fs::remove(fixture.path());
    fs::create_directory(fixture.path());
    require(!store.load(), "state directory accepted");
    fs::remove(fixture.path());
    write(fixture.path(), std::string(256 * 1024 + 1, '#'));
    require(!store.load(), "oversized private state accepted");

    const char* previous = std::getenv("XDG_STATE_HOME");
    const std::optional<std::string> saved = previous != nullptr ? std::optional<std::string>(previous) : std::nullopt;
    ::setenv("XDG_STATE_HOME", "/tmp/noctalia-state-path-test", 1);
    require(
        StateStore::pathFromEnvironment() == "/tmp/noctalia-state-path-test/noctalia-files/state.toml",
        "state path used config directory"
    );
    ::setenv("XDG_STATE_HOME", "relative-invalid", 1);
    bool rejected = false;
    try {
      (void)StateStore::pathFromEnvironment();
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected, "relative state directory accepted");
    ::unsetenv("XDG_STATE_HOME");
    require(
        StateStore::pathFromEnvironment() == fs::path(std::getenv("HOME")) / ".local/state/noctalia-files/state.toml",
        "state fallback is wrong"
    );
    if (saved) {
      ::setenv("XDG_STATE_HOME", saved->c_str(), 1);
    }
  }
} // namespace

int main() {
  try {
    testRoundtrip();
    testSectionMergingAndConflicts();
    testEmptyPlacesSnapshot();
    testInvalidStateAndRecovery();
    testPrivacyAndBounds();
    testPrivateReaderAndPaths();
    std::println("state store checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "state store test: {}", error.what());
    return 1;
  }
}
