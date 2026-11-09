#include "fs/xdg_dirs.h"
#include "test_check.h"

#include <cstdlib>
#include <print>
#include <stdexcept>

namespace {
  bool rejected(std::filesystem::path (*resolve)()) {
    try {
      (void)resolve();
    } catch (const std::invalid_argument&) {
      return true;
    }
    return false;
  }

  void testBaseDirectories() {
    ::setenv("HOME", "/home/xdg-test", 1);
    for (const auto* variable : {"XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"}) {
      ::unsetenv(variable);
    }
    TEST_CHECK(files::configHome() == "/home/xdg-test/.config");
    TEST_CHECK(files::stateHome() == "/home/xdg-test/.local/state");
    TEST_CHECK(files::cacheHome() == "/home/xdg-test/.cache");

    ::setenv("XDG_CACHE_HOME", "", 1);
    TEST_CHECK(files::cacheHome() == "/home/xdg-test/.cache");
    ::setenv("XDG_CACHE_HOME", "/var/cache/xdg-test", 1);
    TEST_CHECK(files::cacheHome() == "/var/cache/xdg-test");
    ::setenv("XDG_CACHE_HOME", "relative/cache", 1);
    TEST_CHECK(rejected(files::cacheHome));

    ::setenv("HOME", "relative-home", 1);
    TEST_CHECK(rejected(files::configHome));
    ::setenv("XDG_CONFIG_HOME", "/etc/xdg-test", 1);
    TEST_CHECK(files::configHome() == "/etc/xdg-test");
    ::unsetenv("HOME");
    TEST_CHECK(rejected(files::stateHome));
  }
} // namespace

int main() {
  testBaseDirectories();
  std::println("xdg dirs checks passed");
  return 0;
}
