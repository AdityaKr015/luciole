#include "app/glib_poll_source.h"
#include "browser/path_completion.h"
#include "fs/file_metadata.h"
#include "platform/terminal_launcher.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <stdexcept>

namespace {
  namespace fs = std::filesystem;
  void require(bool condition, const std::string& message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }
  struct Fixture {
    fs::path path;
    Fixture() {
      const files::GString temp(g_dir_make_tmp("noctalia-navigation-XXXXXX", nullptr));
      require(temp != nullptr, "cannot create fixture");
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
        require(std::chrono::steady_clock::now() < deadline, "navigation service timed out");
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        require(::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) >= 0, "poll failed");
        m_source.dispatch(fds, start);
      }
    }

  private:
    GlibPollSource m_source;
  };
  void identityChecks() {
    const files::GioPtr<GFileInfo> info(g_file_info_new());
    require(!files::readFileIdentity(info.get()), "missing provider identity became known");
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_ID_FILE, "file");
    require(!files::readFileIdentity(info.get()), "a file ID without its filesystem was accepted");
    g_file_info_set_attribute_boolean(info.get(), G_FILE_ATTRIBUTE_ID_FILESYSTEM, true);
    require(!files::readFileIdentity(info.get()), "a mistyped provider identity was accepted");
    g_file_info_set_attribute_string(info.get(), G_FILE_ATTRIBUTE_ID_FILESYSTEM, "filesystem");
    require(
        files::readFileIdentity(info.get()) == files::FileIdentity{"filesystem", "file"},
        "complete provider identity was lost"
    );
  }
  void completionChecks() {
    Fixture fixture;
    Pump pump;
    files::PathCompletion completion;
    const auto base = files::Location::fromPath(fixture.path.native());
    files::PathSuggestions result;
    bool done = false;
    const auto receive = [&](files::PathSuggestions suggestions) {
      result = std::move(suggestions);
      done = true;
    };
    completion.complete("A", base, false, receive);
    pump.until([&] { return done; });
    require(
        result.error.empty() && result.items.size() == 3,
        "completion did not limit to matching folders including directory links"
    );
    for (const auto& item : result.items) {
      require(
          files::resolveLocationText(item.text, base) == item.location,
          "suggestion did not round-trip its operation location"
      );
    }
    done = false;
    completion.complete(".", base, false, receive);
    pump.until([&] { return done; });
    require(
        result.items.size() == 1 && result.items.front().label == ".hidden",
        "explicit hidden prefix did not show its folder"
    );
    done = false;
    completion.complete(base.uri() + "/raw-", base, false, receive);
    pump.until([&] { return done; });
    require(
        result.items.size() == 1
            && result.items.front().text.starts_with("file:")
            && result.items.front().text.contains("%FF"),
        "raw filename was converted into a lossy display address"
    );
    require(
        files::resolveLocationText(result.items.front().text, base) == result.items.front().location,
        "raw URI completion failed to round-trip"
    );
    done = false;
    completion.complete("A%20", base, false, receive);
    pump.until([&] { return done; });
    require(result.items.empty(), "native percent bytes were interpreted as URI escaping");
    done = false;
    completion.complete(base.uri() + "/A%20", base, false, receive);
    pump.until([&] { return done; });
    require(
        result.items.size() == 1 && result.items.front().label == "A space <literal> café",
        "URI escaping was not decoded for matching"
    );
    bool obsolete = false;
    done = false;
    completion.children(base, true, [&](const files::PathSuggestions&) { obsolete = true; });
    completion.complete("Alpha", base, false, receive);
    pump.until([&] { return done; });
    require(!obsolete && result.items.size() == 1, "obsolete completion affected another editor request");
    done = false;
    completion.complete((fixture.path / "missing").native() + "/", base, false, receive);
    pump.until([&] { return done; });
    require(!result.error.empty() && result.items.empty(), "unavailable parent produced misleading suggestions");
    for (int i = 0; i < 270; ++i) {
      fs::create_directory(fixture.path / ("many-" + std::to_string(i)));
    }
    done = false;
    completion.complete("many-", base, false, receive);
    pump.until([&] { return done; });
    require(result.truncated && result.items.size() == 256, "completion results were not bounded explicitly");
    const auto home = files::Location::fromPath(g_get_home_dir());
    require(files::resolveLocationText("~/", base) == home, "home expansion disagreed with navigation");
    require(
        files::resolveLocationText("./Alpha", base) == files::Location::fromPath((fixture.path / "Alpha").native()),
        "relative location parsing disagreed with completion"
    );
    bool rejected = false;
    try {
      (void)files::resolveLocationText(std::string("bad\0name", 8), base);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected, "location parser accepted embedded NUL");
    auto destroyed = std::make_unique<files::PathCompletion>();
    destroyed->children(base, true, [&](const files::PathSuggestions&) { obsolete = true; });
    destroyed.reset();
    done = false;
    completion.children(base, true, receive);
    pump.until([&] { return done; });
    require(!obsolete, "destroyed completion owner received results");
  }
  void terminalChecks() {
    Fixture fixture;
    Pump pump;
    const char* oldPath = g_getenv("PATH");
    const std::string original = oldPath ? oldPath : "";
    const files::GString shell(g_find_program_in_path("sh"));
    require(shell != nullptr, "shell prerequisite unavailable");
    const auto helper = fixture.path / "xdg-terminal-exec";
    const auto log = fixture.path / "terminal.log";
    {
      std::ofstream script(helper);
      script << "#!" << shell.get() << "\nprintf '%s\\0' \"$PWD\" \"$@\" > \"$NOCTALIA_TEST_TERMINAL_LOG\"\n";
    }
    fs::permissions(helper, fs::perms::owner_all);
    g_setenv("PATH", fixture.path.c_str(), true);
    g_setenv("NOCTALIA_TEST_TERMINAL_LOG", log.c_str(), true);
    files::TerminalLauncher launcher;
    g_setenv("PATH", original.c_str(), true);
    require(launcher.available(), "terminal helper not resolved");
    const auto target = fixture.path / "quotes ' $() ; newline\n日本語";
    fs::create_directory(target);
    bool done = false;
    std::string error;
    const auto receive = [&](std::string message) {
      error = std::move(message);
      done = true;
    };
    launcher.open(files::Location::fromPath(target.native()), receive);
    pump.until([&] { return done; });
    require(error.empty(), "terminal helper failed: " + error);
    std::ifstream stream(log, std::ios::binary);
    const std::string output{std::istreambuf_iterator<char>(stream), {}};
    require(
        output == target.native() + std::string(1, '\0') + "--dir=" + target.native() + std::string(1, '\0'),
        "terminal launch split/interpolated arguments or lost the native working directory"
    );
    done = false;
    launcher.open(files::Location::fromUri("sftp://example.invalid/folder"), receive);
    pump.until([&] { return done; });
    require(!error.empty(), "terminal launch accepted a remote URI");
    done = false;
    launcher.open(files::Location::fromPath((fixture.path / "A file").native()), receive);
    pump.until([&] { return done; });
    require(!error.empty(), "terminal launch accepted a regular file");
    g_setenv("PATH", "", true);
    files::TerminalLauncher missing;
    g_setenv("PATH", original.c_str(), true);
    require(!missing.available(), "missing terminal helper was reported available");
    done = false;
    missing.open(files::Location::fromPath(target.native()), receive);
    pump.until([&] { return done; });
    require(!error.empty(), "missing helper did not report a launch error");
    auto destroyed = std::make_unique<files::TerminalLauncher>();
    bool called = false;
    destroyed->open(files::Location::fromUri("sftp://example.invalid/folder"), [&](const std::string&) {
      called = true;
    });
    destroyed.reset();
    done = false;
    missing.open(files::Location::fromPath(target.native()), receive);
    pump.until([&] { return done; });
    require(!called, "destroyed terminal owner received a callback");
    g_unsetenv("NOCTALIA_TEST_TERMINAL_LOG");
  }
} // namespace
int main() {
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
