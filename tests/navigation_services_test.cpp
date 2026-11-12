#include "browser/path_completion.h"
#include "event_loop/glib_poll_source.h"
#include "fs/file_metadata.h"
#include "fs/gio_interop.h"
#include "platform/terminal_launcher.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <stdexcept>

namespace {
  namespace fs = std::filesystem;
  struct Fixture {
    fs::path path;
    Fixture() {
      const luciole::GString temp(g_dir_make_tmp("luciole-navigation-XXXXXX", nullptr));
      TEST_CHECK(temp != nullptr);
      path = temp.get();
      fs::create_directory(path / "Alpha");
      fs::create_directory(path / "A space <literal> café");
      fs::create_directory(path / ".hidden");
      fs::create_directory(path / ("raw-" + std::string(1, static_cast<char>(0xff))));
      fs::create_directory_symlink("Alpha", path / "Alias");
      std::ofstream(path / "A file") << "not a folder";
    }
    ~Fixture() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) >= 0);
        m_source.dispatch(fds, start);
      }
    }

  private:
    GlibPollSource m_source;
  };
  void identityChecks() {
    const luciole::GioPtr<GFileInfo> info(g_file_info_new());
    TEST_CHECK(!luciole::readFileIdentity(info.get()));
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_ID_FILE, "file");
    TEST_CHECK(!luciole::readFileIdentity(info.get()));
    g_file_info_set_attribute_boolean(info.get(), G_FILE_ATTRIBUTE_ID_FILESYSTEM, true);
    TEST_CHECK(!luciole::readFileIdentity(info.get()));
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_ID_FILESYSTEM, "filesystem");
    TEST_CHECK((luciole::readFileIdentity(info.get()) == luciole::FileIdentity{"filesystem", "file"}));
  }
  void completionChecks() {
    Fixture fixture;
    Pump pump;
    luciole::PathCompletion completion;
    const auto base = luciole::Location::fromPath(fixture.path.native());
    luciole::PathSuggestions result;
    bool done = false;
    const auto receive = [&](luciole::PathSuggestions suggestions) {
      result = std::move(suggestions);
      done = true;
    };
    completion.complete("A", base, false, receive);
    pump.until([&] { return done; });
    TEST_CHECK(result.error.empty() && result.items.size() == 3);
    for (const auto& item : result.items) {
      TEST_CHECK(luciole::resolveLocationText(item.text, base) == item.location);
    }
    done = false;
    completion.complete(".", base, false, receive);
    pump.until([&] { return done; });
    TEST_CHECK(result.items.size() == 1 && result.items.front().label == ".hidden");
    done = false;
    completion.complete(base.uri() + "/raw-", base, false, receive);
    pump.until([&] { return done; });
    TEST_CHECK(
        result.items.size() == 1
        && result.items.front().text.starts_with("file:")
        && result.items.front().text.contains("%FF")
    );
    TEST_CHECK(luciole::resolveLocationText(result.items.front().text, base) == result.items.front().location);
    done = false;
    completion.complete("A%20", base, false, receive);
    pump.until([&] { return done; });
    TEST_CHECK(result.items.empty());
    done = false;
    completion.complete(base.uri() + "/A%20", base, false, receive);
    pump.until([&] { return done; });
    TEST_CHECK(result.items.size() == 1 && result.items.front().label == "A space <literal> café");
    bool obsolete = false;
    done = false;
    completion.children(base, true, [&](const luciole::PathSuggestions&) { obsolete = true; });
    completion.complete("Alpha", base, false, receive);
    pump.until([&] { return done; });
    TEST_CHECK(!obsolete && result.items.size() == 1);
    done = false;
    completion.complete((fixture.path / "missing").native() + "/", base, false, receive);
    pump.until([&] { return done; });
    TEST_CHECK(!result.error.empty() && result.items.empty());
    for (int i = 0; i < 270; ++i) {
      fs::create_directory(fixture.path / ("many-" + std::to_string(i)));
    }
    done = false;
    completion.complete("many-", base, false, receive);
    pump.until([&] { return done; });
    TEST_CHECK(result.truncated && result.items.size() == 256);
    const auto home = luciole::Location::fromPath(g_get_home_dir());
    TEST_CHECK(luciole::resolveLocationText("~/", base) == home);
    TEST_CHECK(
        luciole::resolveLocationText("./Alpha", base) == luciole::Location::fromPath((fixture.path / "Alpha").native())
    );
    bool rejected = false;
    try {
      (void)luciole::resolveLocationText(std::string("bad\0name", 8), base);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    TEST_CHECK(rejected);
    auto destroyed = std::make_unique<luciole::PathCompletion>();
    destroyed->children(base, true, [&](const luciole::PathSuggestions&) { obsolete = true; });
    destroyed.reset();
    done = false;
    completion.children(base, true, receive);
    pump.until([&] { return done; });
    TEST_CHECK(!obsolete);
  }
  void terminalChecks() {
    Fixture fixture;
    Pump pump;
    const char* oldPath = g_getenv("PATH");
    const std::string original = oldPath ? oldPath : "";
    const luciole::GString shell(g_find_program_in_path("sh"));
    TEST_CHECK(shell != nullptr);
    const auto helper = fixture.path / "xdg-terminal-exec";
    const auto log = fixture.path / "terminal.log";
    {
      std::ofstream script(helper);
      script << "#!" << shell.get() << "\nprintf '%s\\0' \"$PWD\" \"$@\" > \"$LUCIOLE_TEST_TERMINAL_LOG\"\n";
    }
    fs::permissions(helper, fs::perms::owner_all);
    g_setenv("PATH", fixture.path.c_str(), true);
    g_setenv("LUCIOLE_TEST_TERMINAL_LOG", log.c_str(), true);
    luciole::TerminalLauncher launcher;
    launcher.setCommand(helper.native());
    g_setenv("PATH", original.c_str(), true);
    TEST_CHECK(launcher.available());
    const auto target = fixture.path / "quotes ' $() ; newline\n日本語";
    fs::create_directory(target);
    bool done = false;
    std::string error;
    const auto receive = [&](std::string message) {
      error = std::move(message);
      done = true;
    };
    launcher.open(luciole::Location::fromPath(target.native()), receive);
    pump.until([&] { return done; });
    TEST_CHECK(error.empty());
    std::ifstream stream(log, std::ios::binary);
    const std::string output{std::istreambuf_iterator<char>(stream), {}};
    TEST_CHECK(output == target.native() + std::string(1, '\0') + "--dir=" + target.native() + std::string(1, '\0'));
    done = false;
    launcher.open(luciole::Location::fromUri("sftp://example.invalid/folder"), receive);
    pump.until([&] { return done; });
    TEST_CHECK(!error.empty());
    done = false;
    launcher.open(luciole::Location::fromPath((fixture.path / "A file").native()), receive);
    pump.until([&] { return done; });
    TEST_CHECK(!error.empty());
    g_setenv("PATH", "", true);
    luciole::TerminalLauncher missing;
    g_setenv("PATH", original.c_str(), true);
    TEST_CHECK(!missing.available());
    done = false;
    missing.open(luciole::Location::fromPath(target.native()), receive);
    pump.until([&] { return done; });
    TEST_CHECK(!error.empty());
    auto destroyed = std::make_unique<luciole::TerminalLauncher>();
    bool called = false;
    destroyed->open(luciole::Location::fromUri("sftp://example.invalid/folder"), [&](const std::string&) {
      called = true;
    });
    destroyed.reset();
    done = false;
    missing.open(luciole::Location::fromPath(target.native()), receive);
    pump.until([&] { return done; });
    TEST_CHECK(!called);
    g_unsetenv("LUCIOLE_TEST_TERMINAL_LOG");
  }
} // namespace
int main() {
  g_unsetenv("DISPLAY");
  g_unsetenv("WAYLAND_DISPLAY");
  g_unsetenv("WAYLAND_SOCKET");
  g_unsetenv("TERMINAL");
  try {
    identityChecks();
    completionChecks();
    terminalChecks();
    std::println("navigation services checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "navigation services: {}", error.what());
    return 1;
  }
}
