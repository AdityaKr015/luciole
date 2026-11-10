#include "fs/gio_interop.h"
#include "platform/program_launcher.h"
#include "test_check.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <print>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using namespace luciole;
  using Json = nlohmann::json;
  void write(const fs::path& path, const std::string& data, bool executable = false) {
    std::ofstream(path) << data;
    TEST_CHECK(::chmod(path.c_str(), executable ? 0700 : 0600) == 0);
  }
  std::string hex(const std::string& value) {
    std::string result;
    for (const unsigned char byte : value) {
      result += "0123456789abcdef"[byte >> 4U];
      result += "0123456789abcdef"[byte & 15U];
    }
    return result;
  }
  std::vector<Json> records(const fs::path& log) {
    std::vector<Json> result;
    std::ifstream file(log);
    std::string line;
    while (std::getline(file, line)) {
      result.push_back(Json::parse(line));
    }
    return result;
  }
  Json waitRecord(const fs::path& log, std::size_t count) {
    const auto deadline = g_get_monotonic_time() + 5000000;
    while (true) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      auto result = records(log);
      if (result.size() >= count) {
        return result.at(count - 1);
      }
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
  }
  template <class Work> void fails(FileErrorKind kind, Work work) {
    try {
      work();
      TEST_CHECK(false);
    } catch (const FileErrorException& error) {
      TEST_CHECK(error.error().kind == kind);
    }
  }
  struct Fixture {
    fs::path root, log;
    std::string python, recorder;
    Location cwd;
    std::size_t launches = 0;
    Fixture(fs::path path, std::string interpreter)
        : root(std::move(path)), log(root / "launches.jsonl"), python(std::move(interpreter)),
          cwd(Location::fromPath((root / "working").native())) {
      fs::create_directories(root / "working");
      fs::create_directories(root / "bin");
      recorder = "#!"
          + python
          + "\nimport json,os,sys\n"
            "with open(os.environ['NF_EXEC_TEST_LOG'],'a') as f:\n"
            " f.write(json.dumps({'argv':[os.fsencode(v).hex() for v in sys.argv],"
            "'cwd':os.fsencode(os.getcwd()).hex(),'token':os.getenv('XDG_ACTIVATION_TOKEN'),"
            "'startup':os.getenv('DESKTOP_STARTUP_ID')})+'\\n')\n";
    }
    Json
    run(const PreparedProgram& program, const std::vector<Location>& args = {}, bool terminal = false,
        const std::string& token = {}) {
      launchProgram(program, cwd, args, terminal, token, {});
      return waitRecord(log, ++launches);
    }
  };
  void testPrograms(Fixture& f) {
    const auto file = f.root / "runner ; $(touch CANARY).py";
    write(file, f.recorder, true);
    const auto location = Location::fromPath(file.native());
    const auto program = prepareProgram(location, {});
    TEST_CHECK(!program.desktop && records(f.log).empty());
    const auto raw = (f.root / (std::string("raw-") + static_cast<char>(0xff) + ".txt")).native();
    const auto strange = (f.root / "space café #?% ' $(touch CANARY).txt").native();
    const auto args = std::vector{Location::fromPath(raw), Location::fromPath(strange)};
    const auto result = f.run(program, args, false, "fresh-token");
    TEST_CHECK(result["argv"] == Json::array({hex(file.native()), hex(raw), hex(strange)}));
    TEST_CHECK(result["cwd"] == hex(*f.cwd.nativePath()) && result["token"] == "fresh-token");
    TEST_CHECK(result["startup"].is_null() && !fs::exists(f.root / "working/CANARY"));
    TEST_CHECK(f.run(program)["token"].is_null());
    const auto link = f.root / "program-link";
    fs::create_symlink(file, link);
    TEST_CHECK(f.run(prepareProgram(Location::fromPath(link.native()), {}))["argv"][0] == hex(link.native()));
    const auto helper = f.root / "bin/xdg-terminal-exec";
    write(helper, f.recorder, true);
    const auto terminal = f.run(program, args, true);
    TEST_CHECK(
        terminal["argv"]
        == Json::array(
            {hex(helper.native()), hex("--dir=" + *f.cwd.nativePath()), hex("--"), hex(file.native()), hex(raw),
             hex(strange)}
        )
    );
    const CancelToken cancelled;
    cancelled.cancel();
    fails(FileErrorKind::Cancelled, [&] { (void)prepareProgram(location, cancelled); });
    fails(FileErrorKind::Cancelled, [&] { launchProgram(program, f.cwd, {}, false, {}, cancelled); });
    fails(FileErrorKind::Unsupported, [&] { f.run(program, {Location::fromUri("sftp://example.invalid/file")}); });
    fails(FileErrorKind::InvalidArgument, [&] { f.run(program, std::vector<Location>(4097, location)); });
    write(file, f.recorder + "# changed length\n", true);
    fails(FileErrorKind::Changed, [&] { f.run(program); });
    const auto current = prepareProgram(location, {});
    fs::rename(file, f.root / "old-program");
    write(file, f.recorder, true);
    fails(FileErrorKind::Changed, [&] { f.run(current); });
    const auto linked = prepareProgram(Location::fromPath(link.native()), {});
    fs::remove(link);
    fs::create_symlink(f.root / "old-program", link);
    fails(FileErrorKind::Changed, [&] { f.run(linked); });
  }
  void testRefusals(Fixture& f) {
    const auto file = f.root / "not-executable.py";
    write(file, f.recorder);
    fails(FileErrorKind::Denied, [&] { (void)prepareProgram(Location::fromPath(file.native()), {}); });
    write(file, "plain text, even with the executable bit", true);
    fails(FileErrorKind::Unsupported, [&] { (void)prepareProgram(Location::fromPath(file.native()), {}); });
    write(file, "#!/luciole-execution-test-missing-interpreter\n", true);
    fails(FileErrorKind::Other, [&] { f.run(prepareProgram(Location::fromPath(file.native()), {})); });
    for (const auto& item :
         {Location::fromUri("sftp://example.invalid/file"), Location::trash(), f.cwd,
          Location::fromPath("/dev/null")}) {
      fails(FileErrorKind::Unsupported, [&] { (void)prepareProgram(item, {}); });
    }
    fails(FileErrorKind::Missing, [&] { (void)prepareProgram(Location::fromPath((f.root / "missing").native()), {}); });
  }
  void testDesktop(Fixture& f) {
    const auto worker = f.root / "desktop-recorder.py";
    write(worker, f.recorder);
    const auto file = f.root / "launch café %.desktop";
    auto desktop = [&](const std::string& fields) {
      write(file, "[Desktop Entry]\nType=Application\nName=Literal <launcher>\n" + fields);
      return prepareProgram(Location::fromPath(file.native()), {});
    };
    const auto prefix = "Exec=" + f.python + " " + worker.native();
    auto program = desktop(prefix + " %U\nPath=" + *f.cwd.nativePath() + "\n");
    TEST_CHECK(program.desktop && program.name == "Literal <launcher>");
    const auto raw = Location::fromPath((f.root / (std::string("raw-") + static_cast<char>(0xff))).native());
    const auto remote = Location::fromUri("davs://example.invalid/files/caf%C3%A9%20%23.txt");
    auto record = f.run(program, {raw, remote}, false, "desktop-token");
    TEST_CHECK(record["argv"] == Json::array({hex(worker.native()), hex(*raw.nativePath()), hex(remote.uri())}));
    TEST_CHECK(record["token"] == "desktop-token" && record["startup"].is_null());
    program = desktop(prefix + " %F\nPath=" + *f.cwd.nativePath() + "\n");
    record = f.run(program, {raw});
    TEST_CHECK(record["argv"] == Json::array({hex(worker.native()), hex(*raw.nativePath())}));
    program = desktop(prefix + " %k %%k %c\n");
    record = f.run(program);
    TEST_CHECK(
        record["argv"]
        == Json::array(
            {hex(worker.native()), hex(Location::fromPath(file.native()).uri()), hex("%k"), hex(program.name)}
        )
    );
    fails(FileErrorKind::Unsupported, [&] { f.run(program, {}, true); });
    write(file, "[Desktop Entry]\nType=Application\nName=Changed\nExec=/luciole-test-missing\n");
    fails(FileErrorKind::Changed, [&] { f.run(program); });
    for (const auto& text :
         {"not a desktop file", "[Desktop Entry]\nType=Link\nURL=https://example.invalid\n",
          "[Desktop Entry]\nType=Application\nName=Absent\nDBusActivatable=true\n"}) {
      write(file, text);
      bool refused = false;
      try {
        (void)prepareProgram(Location::fromPath(file.native()), {});
      } catch (const FileErrorException&) {
        refused = true;
      }
      TEST_CHECK(refused);
    }
  }
  void testScripts(Fixture& f) {
    TEST_CHECK(listScripts({}).items.empty());
    const auto root = ensureScriptsDirectory({});
    TEST_CHECK(root == scriptsDirectory());
    const fs::path directory(*root.nativePath());
    write(directory / "Normal.py", f.recorder, true);
    write(directory / ".hidden.py", f.recorder, true);
    write(directory / "not-executable", f.recorder);
    write(directory / "unsafe.py", f.recorder, true);
    TEST_CHECK(::chmod((directory / "unsafe.py").c_str(), 0777) == 0);
    fs::create_directory(directory / "subfolder");
    write(f.root / "external.py", f.recorder, true);
    fs::create_symlink(f.root / "external.py", directory / "Linked.py");
    auto listing = listScripts({});
    TEST_CHECK(!listing.error && listing.items.size() == 2 && listing.items[0].name == "Linked.py");
    const auto args = std::vector{Location::fromPath((f.root / "some file.txt").native())};
    TEST_CHECK(f.run(listing.items[0], args)["argv"][1] == hex(*args[0].nativePath()));
    TEST_CHECK(::chmod(directory.c_str(), 0777) == 0);
    fails(FileErrorKind::Denied, [&] { f.run(listing.items[0], args); });
    TEST_CHECK(listScripts({}).error->kind == FileErrorKind::Denied);
    TEST_CHECK(::chmod(directory.c_str(), 0700) == 0);
    listing = listScripts({});
    fs::rename(directory, directory.parent_path() / "old-scripts");
    fs::create_directory(directory);
    fails(FileErrorKind::Changed, [&] { f.run(listing.items[0]); });
    for (int i = 0; i < 70; ++i) {
      write(directory / ("script-" + std::to_string(i)), f.recorder, true);
    }
    listing = listScripts({});
    TEST_CHECK(listing.items.size() == 64 && listing.truncated);
    const CancelToken cancelled;
    cancelled.cancel();
    TEST_CHECK(listScripts(cancelled).error->kind == FileErrorKind::Cancelled);
  }
} // namespace

int main() {
  char temporary[] = "/tmp/luciole-program-test-XXXXXX";
  TEST_CHECK(mkdtemp(temporary));
  const fs::path root(temporary);
  const luciole::GString python(g_find_program_in_path("python3"));
  TEST_CHECK(python);
  g_setenv("HOME", temporary, true);
  g_setenv("XDG_DATA_HOME", (root / "data").c_str(), true);
  g_setenv("XDG_CONFIG_HOME", (root / "config").c_str(), true);
  g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/luciole-program-test-missing-bus", true);
  g_setenv("NF_EXEC_TEST_LOG", (root / "launches.jsonl").c_str(), true);
  g_setenv("XDG_ACTIVATION_TOKEN", "must-not-inherit", true);
  g_setenv("DESKTOP_STARTUP_ID", "must-not-inherit", true);
  const std::string search = (root / "bin").native() + ":" + g_getenv("PATH");
  g_setenv("PATH", search.c_str(), true);
  g_unsetenv("GIO_LAUNCH_DESKTOP");
  g_unsetenv("DISPLAY");
  g_unsetenv("WAYLAND_DISPLAY");
  try {
    Fixture fixture(root, python.get());
    testPrograms(fixture);
    testRefusals(fixture);
    testDesktop(fixture);
    testScripts(fixture);
    fs::remove_all(root);
    std::println("program launcher checks passed");
  } catch (const std::exception& error) {
    std::println(stderr, "{}", error.what());
    fs::remove_all(root);
    return 1;
  }
}
