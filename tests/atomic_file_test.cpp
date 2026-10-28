#include "config/atomic_file.h"

#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <print>
#include <stdexcept>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using files::AtomicWriteState;

  void require(bool condition, const std::string& message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  struct Fixture {
    fs::path directory;
    Fixture() {
      auto pattern = (fs::temp_directory_path() / "noctalia-atomic-test-XXXXXX").native();
      const char* created = ::mkdtemp(pattern.data());
      require(created != nullptr, "cannot create atomic writer fixture");
      directory = created;
    }
    ~Fixture() {
      std::error_code ignored;
      fs::remove_all(directory, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
  };

  std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  void plainWrite(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
    require(output.good(), "cannot write atomic fixture");
  }

  files::AtomicWriteResult save(const fs::path& path, const std::string& text) {
    return files::updateTextFileAtomic(
        path, [&](const auto&) -> std::expected<std::optional<std::string>, std::string> { return text; }
    );
  }

  void noTemporaryFiles(const fs::path& directory) {
    for (const auto& item : fs::directory_iterator(directory)) {
      const auto name = item.path().filename().native();
      require(!name.starts_with(".settings.toml.tmp."), "writer leaked its temporary file");
    }
  }

  void testDurableReplacementAndNoop() {
    Fixture fixture;
    const auto invalid = fs::path((fixture.directory / "settings.toml").native() + std::string("\0suffix", 7));
    require(
        save(invalid, "invalid").state == AtomicWriteState::NOT_COMMITTED
            && !fs::exists(fixture.directory / "settings.toml"),
        "NUL path was truncated to another target"
    );
    const auto parent = fixture.directory / "new" / "nested";
    const auto path = parent / "settings.toml";
    auto result = save(path, "first");
    require(
        result.state == AtomicWriteState::DURABLE && result.error.empty() && read(path) == "first",
        "first durable write failed"
    );
    struct stat info{};
    require(::stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "settings were not private");
    require(::stat(parent.c_str(), &info) == 0 && (info.st_mode & 0777) == 0700, "new directory was not private");
    plainWrite(parent / "settings.toml.tmp", "unrelated fixed temporary name");
    result = save(path, "second");
    require(result.state == AtomicWriteState::DURABLE && read(path) == "second", "replacement failed");
    require(
        read(parent / "settings.toml.tmp") == "unrelated fixed temporary name", "writer touched an unowned temp file"
    );
    require(::stat(path.c_str(), &info) == 0, "cannot inspect saved file");
    const auto inode = info.st_ino;
    result = save(path, "second");
    require(
        result.state == AtomicWriteState::UNCHANGED && ::stat(path.c_str(), &info) == 0 && info.st_ino == inode,
        "no-op save replaced the file"
    );
    require(
        save(path, std::string(256 * 1024 + 1, 'x')).state == AtomicWriteState::NOT_COMMITTED && read(path) == "second",
        "oversized replacement changed settings"
    );
    noTemporaryFiles(parent);
  }

  void testRejectedTargetsAndConflicts() {
    Fixture fixture;
    const auto path = fixture.directory / "settings.toml";
    const auto outside = fixture.directory / "base.toml";
    plainWrite(outside, "base");
    fs::create_symlink(outside, path);
    require(
        save(path, "candidate").state == AtomicWriteState::NOT_COMMITTED
            && read(outside) == "base"
            && fs::is_symlink(path),
        "writer followed a symlink"
    );
    fs::remove(path);
    fs::create_hard_link(outside, path);
    require(
        save(path, "candidate").state == AtomicWriteState::NOT_COMMITTED && read(outside) == "base",
        "hard link accepted"
    );
    fs::remove(path);
    fs::create_directory(path);
    require(save(path, "candidate").state == AtomicWriteState::NOT_COMMITTED, "directory accepted");
    fs::remove(path);
    require(::mkfifo(path.c_str(), 0600) == 0, "cannot create FIFO");
    require(save(path, "candidate").state == AtomicWriteState::NOT_COMMITTED, "FIFO accepted");
    fs::remove(path);
    plainWrite(path, "old");
    require(::chmod(path.c_str(), 0444) == 0, "cannot protect settings fixture");
    require(
        save(path, "candidate").state == AtomicWriteState::NOT_COMMITTED && read(path) == "old",
        "read-only target replaced"
    );
    ::chmod(path.c_str(), 0600);
    const auto conflict = files::updateTextFileAtomic(
        path, [&](const auto& current) -> std::expected<std::optional<std::string>, std::string> {
          require(current == "old", "writer did not read existing contents");
          plainWrite(path, "external edit");
          return "candidate";
        }
    );
    require(
        conflict.state == AtomicWriteState::NOT_COMMITTED && read(path) == "external edit",
        "detected external edit overwritten"
    );
    fs::remove(path);
    const auto appeared = files::updateTextFileAtomic(
        path, [&](const auto& current) -> std::expected<std::optional<std::string>, std::string> {
          require(!current, "missing target was not absent");
          plainWrite(path, "external new file");
          return "candidate";
        }
    );
    require(
        appeared.state == AtomicWriteState::NOT_COMMITTED && read(path) == "external new file",
        "new external target replaced"
    );
    noTemporaryFiles(fixture.directory);
  }

  void testLockAndFailureCleanup() {
    Fixture fixture;
    const auto path = fixture.directory / "settings.toml";
    require(save(path, "old").state == AtomicWriteState::DURABLE, "cannot create lock fixture");
    const int lock = ::open((fixture.directory / ".settings.toml.lock").c_str(), O_RDWR | O_CLOEXEC);
    require(lock >= 0 && ::flock(lock, LOCK_EX | LOCK_NB) == 0, "cannot hold settings lock");
    const auto busy = save(path, "blocked");
    require(
        busy.state == AtomicWriteState::NOT_COMMITTED && read(path) == "old" && !busy.error.empty(),
        "busy writer changed settings"
    );
    const auto contender = ::fork();
    require(contender >= 0, "cannot fork lock contender");
    if (contender == 0) {
      const auto result = save(path, "blocked child");
      ::_exit(result.state == AtomicWriteState::NOT_COMMITTED ? 0 : 1);
    }
    int contenderStatus = 0;
    require(
        ::waitpid(contender, &contenderStatus, 0) == contender
            && WIFEXITED(contenderStatus)
            && WEXITSTATUS(contenderStatus) == 0,
        "another process bypassed the write lock"
    );
    ::close(lock);
    require(save(path, "after lock").state == AtomicWriteState::DURABLE, "lock was not released");

    // A real storage failure before rename must preserve the old file and
    // remove this transaction's temporary file. Limits affect only the child.
    const auto child = ::fork();
    require(child >= 0, "cannot fork storage failure fixture");
    if (child == 0) {
      std::signal(SIGXFSZ, SIG_IGN);
      const rlimit limit{.rlim_cur = 0, .rlim_max = 0};
      if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
        ::_exit(2);
      }
      const auto result = save(path, "must fail while writing");
      ::_exit(result.state == AtomicWriteState::NOT_COMMITTED ? 0 : 1);
    }
    int status = 0;
    require(
        ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "storage failure was not reported"
    );
    require(read(path) == "after lock", "failed write changed old contents");
    noTemporaryFiles(fixture.directory);
    fs::remove(fixture.directory / ".settings.toml.lock");
    const auto untouched = fixture.directory / "lock-target";
    plainWrite(untouched, "leave me alone");
    fs::create_symlink(untouched, fixture.directory / ".settings.toml.lock");
    require(
        save(path, "candidate").state == AtomicWriteState::NOT_COMMITTED && read(untouched) == "leave me alone",
        "writer followed a lock symlink"
    );
  }
} // namespace

int main() {
  try {
    testDurableReplacementAndNoop();
    testRejectedTargetsAndConflicts();
    testLockAndFailureCleanup();
    std::println("atomic file checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "atomic file test: {}", error.what());
    return 1;
  }
}
