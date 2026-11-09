#include "event_loop/glib_poll_source.h"
#include "operations/transfer_service.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using files::TransferAction;
  using files::TransferConflictChoice;
  using files::TransferOutcome;
  void write(const fs::path& path, const std::string& text) { std::ofstream(path) << text; }
  std::string read(const fs::path& path) {
    std::ifstream input(path);
    return {(std::istreambuf_iterator<char>(input)), {}};
  }
  files::Location location(const fs::path& path) { return files::Location::fromPath(path.native()); }
  struct Fixture {
    fs::path root;
    explicit Fixture(const std::string& parent = "/tmp") {
      std::string name = parent + "/noctalia-transfer-XXXXXX";
      TEST_CHECK(::mkdtemp(name.data()) != nullptr);
      root = name;
      fs::create_directory(root / "source");
      fs::create_directory(root / "destination");
    }
    ~Fixture() {
      std::error_code error;
      for (const auto& item :
           fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, error)) {
        if (item.is_directory() && !item.is_symlink()) {
          fs::permissions(item.path(), fs::perms::owner_all, fs::perm_options::add, error);
        }
      }
      fs::remove_all(root, error);
    }
    fs::path source(const std::string& name) const { return root / "source" / name; }
    fs::path destination(const std::string& name) const { return root / "destination" / name; }
    void clean() const {
      for (const auto& item : fs::recursive_directory_iterator(root)) {
        TEST_CHECK(!item.path().filename().native().starts_with(".noctalia-files-transfer-"));
      }
    }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto first = m_source.addPollFds(fds);
        TEST_CHECK(::poll(fds.data(), fds.size(), 1) >= 0);
        m_source.dispatch(fds, first);
      }
    }
    void
    run(files::TransferService& service, std::vector<files::Location> sources, const fs::path& destination,
        TransferAction action, std::function<TransferConflictChoice(const files::TransferConflict&)> choose = {}) {
      TEST_CHECK(service.start({std::move(sources), location(destination), action}));
      std::uint64_t handled = 0;
      until([&] {
        if (const auto conflict = service.conflict(); conflict && conflict->id != handled) {
          handled = conflict->id;
          TEST_CHECK(!service.resolve(conflict->id + 100000, TransferConflictChoice::Replace));
          const auto decision = choose ? choose(*conflict) : TransferConflictChoice::Cancel;
          TEST_CHECK(service.resolve(conflict->id, decision));
        }
        return !service.busy();
      });
    }

  private:
    GlibPollSource m_source;
  };
  void testMoveAndCopy(Pump& pump) {
    Fixture fixture;
    files::TransferService service;
    write(fixture.source("file.txt"), "payload");
    fs::create_directories(fixture.source("folder/sub"));
    write(fixture.source("folder/sub/.hidden"), "nested");
    const bool attributes =
        ::setxattr(fixture.source("folder/sub/.hidden").c_str(), "user.noctalia-test", "metadata", 8, 0) == 0;
    fs::create_symlink("../../outside", fixture.source("folder/link"));
    write(fixture.root / "outside", "outside must survive");
    pump.run(
        service, {location(fixture.source("file.txt")), location(fixture.source("folder"))}, fixture.destination(""),
        TransferAction::Move
    );
    TEST_CHECK(service.results().size() == 2 && std::ranges::all_of(service.results(), [](const auto& result) {
                 return result.outcome == TransferOutcome::Moved && result.error.empty();
               }));
    TEST_CHECK(
        !fs::exists(fixture.source("file.txt"))
        && read(fixture.destination("file.txt")) == "payload"
        && read(fixture.destination("folder/sub/.hidden")) == "nested"
    );
    TEST_CHECK(
        fs::is_symlink(fixture.destination("folder/link")) && read(fixture.root / "outside") == "outside must survive"
    );
    pump.run(service, {location(fixture.destination("folder"))}, fixture.source(""), TransferAction::Copy);
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Copied
        && read(fixture.source("folder/sub/.hidden")) == "nested"
        && fs::read_symlink(fixture.source("folder/link")) == "../../outside"
    );
    if (attributes) {
      char value[16]{};
      TEST_CHECK(
          ::getxattr(fixture.source("folder/sub/.hidden").c_str(), "user.noctalia-test", value, sizeof(value)) == 8
          && std::string(value, 8) == "metadata"
      );
    }
    const std::string raw = std::string("raw-") + static_cast<char>(0xff);
    write(fixture.source(raw), "raw filename");
    pump.run(service, {location(fixture.source(raw))}, fixture.destination(""), TransferAction::Copy);
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Copied && read(fixture.destination(raw)) == "raw filename"
    );
    fixture.clean();
  }
  void testConflicts(Pump& pump) {
    Fixture fixture;
    files::TransferService service;
    write(fixture.source("image.png"), "incoming");
    write(fixture.destination("image.png"), "existing");
    pump.run(
        service, {location(fixture.source("image.png"))}, fixture.destination(""), TransferAction::Copy,
        [](const auto&) { return TransferConflictChoice::Skip; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Skipped && read(fixture.destination("image.png")) == "existing"
    );
    write(fixture.destination("image (1).png"), "occupied");
    pump.run(
        service, {location(fixture.source("image.png"))}, fixture.destination(""), TransferAction::Copy,
        [](const auto&) { return TransferConflictChoice::KeepBoth; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Copied
        && read(fixture.destination("image (2).png")) == "incoming"
        && read(fixture.destination("image.png")) == "existing"
    );
    pump.run(
        service, {location(fixture.source("image.png"))}, fixture.destination(""), TransferAction::Move,
        [](const auto&) { return TransferConflictChoice::Replace; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Moved
        && !fs::exists(fixture.source("image.png"))
        && read(fixture.destination("image.png")) == "incoming"
    );
    fs::create_directories(fixture.source("folder"));
    fs::create_directories(fixture.destination("folder"));
    write(fixture.source("folder/new"), "new");
    write(fixture.destination("folder/old"), "old");
    pump.run(
        service, {location(fixture.source("folder"))}, fixture.destination(""), TransferAction::Copy,
        [](const auto&) { return TransferConflictChoice::Replace; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Copied
        && read(fixture.destination("folder/new")) == "new"
        && !fs::exists(fixture.destination("folder/old"))
        && fs::exists(fixture.source("folder/new"))
    );
    fs::create_symlink(fixture.root / "protected", fixture.destination("link"));
    write(fixture.root / "protected", "protected");
    write(fixture.source("link"), "regular incoming");
    pump.run(
        service, {location(fixture.source("link"))}, fixture.destination(""), TransferAction::Copy,
        [](const auto&) { return TransferConflictChoice::Replace; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Copied
        && !fs::is_symlink(fixture.destination("link"))
        && read(fixture.root / "protected") == "protected"
    );
    fixture.clean();
  }
  void testGuardsAndRaces(Pump& pump) {
    Fixture fixture;
    files::TransferService service;
    fs::create_directories(fixture.source("tree/child"));
    write(fixture.source("tree/file"), "tree");
    fs::create_directory_symlink(fixture.source("tree/child"), fixture.root / "alias");
    pump.run(service, {location(fixture.source("tree"))}, fixture.root / "alias", TransferAction::Copy);
    TEST_CHECK(service.results()[0].outcome == TransferOutcome::Failed && fs::is_empty(fixture.source("tree/child")));
    pump.run(
        service, {location(fixture.source("tree/file")), location(fixture.source("tree"))}, fixture.destination(""),
        TransferAction::Move
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Skipped
        && service.results()[1].outcome == TransferOutcome::Moved
        && read(fixture.destination("tree/file")) == "tree"
    );
    write(fixture.source("race.txt"), "incoming");
    write(fixture.destination("race.txt"), "initial destination");
    int decisions = 0;
    pump.run(
        service, {location(fixture.source("race.txt"))}, fixture.destination(""), TransferAction::Copy,
        [&](const auto&) {
          if (++decisions == 1) {
            write(fixture.root / "replacement", "changed destination");
            fs::rename(fixture.root / "replacement", fixture.destination("race.txt"));
            return TransferConflictChoice::Replace;
          }
          return TransferConflictChoice::Skip;
        }
    );
    TEST_CHECK(
        decisions == 2
        && service.results()[0].outcome == TransferOutcome::Skipped
        && read(fixture.destination("race.txt")) == "changed destination"
    );
    pump.run(
        service, {location(fixture.source("race.txt"))}, fixture.destination(""), TransferAction::Move,
        [&](const auto&) {
          write(fixture.root / "new source", "replaced source");
          fs::rename(fixture.root / "new source", fixture.source("race.txt"));
          return TransferConflictChoice::KeepBoth;
        }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Failed
        && read(fixture.source("race.txt")) == "replaced source"
        && !fs::exists(fixture.destination("race (1).txt"))
    );
    TEST_CHECK(::mkfifo(fixture.source("pipe").c_str(), 0600) == 0);
    pump.run(
        service, {location(fixture.source("pipe")), location(fixture.source("race.txt"))}, fixture.destination(""),
        TransferAction::Copy, [](const auto&) { return TransferConflictChoice::Skip; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Failed
        && service.results()[1].outcome == TransferOutcome::Skipped
    );
    fixture.clean();
  }
  void testCancellationAndNumbering(Pump& pump) {
    Fixture fixture;
    files::TransferService service;
    write(fixture.source("same.txt"), "same");
    pump.run(service, {location(fixture.source("same.txt"))}, fixture.source(""), TransferAction::Copy);
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Copied && read(fixture.source("same (1).txt")) == "same"
    );
    const std::string longName(255, 'a');
    write(fixture.source(longName), "long");
    pump.run(service, {location(fixture.source(longName))}, fixture.source(""), TransferAction::Copy);
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Copied
        && read(fixture.source(std::string(251, 'a') + " (1)")) == "long"
    );
    write(fixture.destination("same.txt"), "existing");
    pump.run(
        service, {location(fixture.source("same.txt")), location(fixture.source("same (1).txt"))},
        fixture.destination(""), TransferAction::Move, [](const auto&) { return TransferConflictChoice::Cancel; }
    );
    TEST_CHECK(
        std::ranges::all_of(
            service.results(), [](const auto& item) { return item.outcome == TransferOutcome::Cancelled; }
        )
        && read(fixture.destination("same.txt")) == "existing"
    );
    write(fixture.source("large"), "");
    fs::resize_file(fixture.source("large"), 1024ULL * 1024 * 1024);
    TEST_CHECK(
        service.start({{location(fixture.source("large"))}, location(fixture.destination("")), TransferAction::Copy})
    );
    TEST_CHECK(!service.start({{location(fixture.source("same.txt"))}, location(fixture.destination(""))}));
    pump.until([&] {
      if (service.bytes() != 0) {
        service.cancel();
      }
      return !service.busy();
    });
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Cancelled
        && fs::exists(fixture.source("large"))
        && !fs::exists(fixture.destination("large"))
    );
    fixture.clean();
  }
  void testCrossFilesystem(Pump& pump) {
    if (::access("/dev/shm", W_OK) != 0) {
      std::println("Cross-filesystem check skipped: /dev/shm unavailable");
      return;
    }
    Fixture source;
    Fixture target("/dev/shm");
    struct stat a{}, b{};
    TEST_CHECK(::stat(source.root.c_str(), &a) == 0 && ::stat(target.root.c_str(), &b) == 0);
    if (a.st_dev == b.st_dev) {
      std::println("Cross-filesystem check skipped: fixtures share a filesystem");
      return;
    }
    fs::create_directories(source.source("tree/sub"));
    write(source.source("tree/sub/data"), "cross filesystem");
    fs::create_hard_link(source.source("tree/sub/data"), source.source("tree/sub/linked"));
    fs::create_symlink("missing", source.source("tree/broken"));
    files::TransferService service;
    pump.run(service, {location(source.source("tree"))}, target.destination(""), TransferAction::Move);
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Moved
        && !fs::exists(source.source("tree"))
        && read(target.destination("tree/sub/linked")) == "cross filesystem"
        && fs::is_symlink(target.destination("tree/broken"))
    );
    fs::create_directories(source.source("merge/sub"));
    fs::create_directories(target.destination("merge/sub"));
    write(source.source("merge/sub/value"), "cross-filesystem merge");
    write(target.destination("merge/sub/keep"), "existing");
    pump.run(
        service, {location(source.source("merge"))}, target.destination(""), TransferAction::Move,
        [](const auto&) { return TransferConflictChoice::Merge; }
    );
    TEST_CHECK(service.results().front().outcome == TransferOutcome::Moved && !fs::exists(source.source("merge")));
    TEST_CHECK(
        read(target.destination("merge/sub/value")) == "cross-filesystem merge"
        && read(target.destination("merge/sub/keep")) == "existing"
    );
    source.clean();
    target.clean();
  }
  void finish(
      Pump& pump, files::TransferService& service,
      const std::function<TransferConflictChoice(const files::TransferConflict&)>& choose, bool all = false
  ) {
    std::uint64_t handled = 0;
    pump.until([&] {
      if (const auto conflict = service.conflict(); conflict && conflict->id != handled) {
        handled = conflict->id;
        TEST_CHECK(service.resolve(conflict->id, choose(*conflict), all));
      }
      return !service.busy();
    });
  }
  void testMerge(Pump& pump) {
    for (const auto action : {TransferAction::Copy, TransferAction::Move}) {
      Fixture f;
      files::TransferService service;
      fs::create_directories(f.source("tree/sub"));
      fs::create_directories(f.destination("tree/sub"));
      write(f.source("tree/sub/value"), "incoming");
      write(f.destination("tree/sub/value"), "old value");
      write(f.destination("tree/keep"), "destination-only");
      fs::create_symlink("missing", f.source("tree/broken"));
      TEST_CHECK(::chmod(f.destination("tree").c_str(), 0750) == 0);
      int prompts = 0;
      pump.run(service, {location(f.source("tree"))}, f.destination(""), action, [&](const auto& conflict) {
        ++prompts;
        if (conflict.sourceDirectory && conflict.destinationDirectory) {
          TEST_CHECK(conflict.canMerge);
          return TransferConflictChoice::Merge;
        }
        TEST_CHECK(!conflict.canMerge && conflict.destination == location(f.destination("tree/sub/value")));
        return TransferConflictChoice::Replace;
      });
      TEST_CHECK(
          prompts == 3
          && service.results().front().outcome
              == (action == TransferAction::Copy ? TransferOutcome::Copied : TransferOutcome::Moved)
      );
      TEST_CHECK(
          read(f.destination("tree/sub/value")) == "incoming" && read(f.destination("tree/keep")) == "destination-only"
      );
      TEST_CHECK(fs::is_symlink(f.destination("tree/broken")));
      TEST_CHECK(fs::exists(f.source("tree")) == (action == TransferAction::Copy));
      TEST_CHECK((fs::status(f.destination("tree")).permissions() & fs::perms::all) == fs::perms(0750));
      TEST_CHECK(!service.retryFailed() && service.report().contains("/tree/sub/value"));
      f.clean();
    }
  }
  void testMergeSkipAndCancel(Pump& pump) {
    for (const auto choice : {TransferConflictChoice::Skip, TransferConflictChoice::Cancel}) {
      Fixture f;
      files::TransferService service;
      fs::create_directories(f.source("tree"));
      fs::create_directories(f.destination("tree"));
      write(f.source("tree/a-done"), "done");
      write(f.source("tree/z-conflict"), "incoming");
      write(f.destination("tree/z-conflict"), "old");
      pump.run(
          service, {location(f.source("tree"))}, f.destination(""), TransferAction::Move,
          [&](const auto& conflict) { return conflict.canMerge ? TransferConflictChoice::Merge : choice; }
      );
      TEST_CHECK(service.results().front().outcome == TransferOutcome::Partial);
      TEST_CHECK(read(f.destination("tree/a-done")) == "done" && !fs::exists(f.source("tree/a-done")));
      TEST_CHECK(read(f.destination("tree/z-conflict")) == "old" && read(f.source("tree/z-conflict")) == "incoming");
      TEST_CHECK(service.retryable() == 0 && service.results().front().children.size() == 2);
      f.clean();
    }
  }
  void testScopedChoices(Pump& pump) {
    Fixture f;
    files::TransferService service;
    for (const auto* folder : {"a", "b"}) {
      fs::create_directories(f.source(folder));
      fs::create_directories(f.destination(folder));
      write(f.source(std::string(folder) + "/value"), "incoming");
      write(f.destination(std::string(folder) + "/value"), "existing");
    }
    TEST_CHECK(service.start(
        {{location(f.source("a")), location(f.source("b"))}, location(f.destination("")), TransferAction::Copy}
    ));
    int prompts = 0;
    finish(
        pump, service,
        [&](const auto& conflict) {
          ++prompts;
          TEST_CHECK(conflict.canApplyToAll && !conflict.scopeLabel.empty());
          return conflict.canMerge ? TransferConflictChoice::Merge : TransferConflictChoice::KeepBoth;
        },
        true
    );
    TEST_CHECK(
        prompts == 2 && read(f.destination("a/value")) == "existing" && read(f.destination("b/value (1)")) == "incoming"
    );
    // New jobs do not inherit any policy.
    prompts = 0;
    pump.run(service, {location(f.source("a"))}, f.destination(""), TransferAction::Copy, [&](const auto&) {
      ++prompts;
      return TransferConflictChoice::Skip;
    });
    TEST_CHECK(prompts == 1);
    write(f.source("x"), "incoming x");
    write(f.source("y"), "incoming y");
    write(f.destination("x"), "old x");
    write(f.destination("y"), "old y");
    TEST_CHECK(service.start(
        {{location(f.source("x")), location(f.source("y"))}, location(f.destination("")), TransferAction::Copy}
    ));
    std::uint64_t handled = 0;
    prompts = 0;
    pump.until([&] {
      if (const auto c = service.conflict(); c && c->id != handled) {
        handled = c->id;
        ++prompts;
        if (prompts == 1) {
          write(f.root / "replacement", "foreign");
          fs::rename(f.root / "replacement", f.destination("x"));
          TEST_CHECK(service.resolve(c->id, TransferConflictChoice::Replace, true));
        } else {
          TEST_CHECK(service.resolve(c->id, TransferConflictChoice::Skip));
        }
      }
      return !service.busy();
    });
    TEST_CHECK(prompts == 2 && read(f.destination("x")) == "foreign" && read(f.destination("y")) == "incoming y");
    f.clean();
  }
  void testMergeBindings(Pump& pump) {
    Fixture f;
    files::TransferService service;
    fs::create_directories(f.source("tree"));
    fs::create_directories(f.destination("tree"));
    write(f.source("tree/value"), "incoming");
    write(f.destination("tree/old"), "existing");
    int prompts = 0;
    pump.run(service, {location(f.source("tree"))}, f.destination(""), TransferAction::Copy, [&](const auto&) {
      if (++prompts == 1) {
        fs::rename(f.destination("tree"), f.destination("held"));
        fs::create_directory(f.destination("tree"));
        write(f.destination("tree/foreign"), "foreign");
        return TransferConflictChoice::Merge;
      }
      return TransferConflictChoice::Skip;
    });
    TEST_CHECK(
        prompts == 2 && !fs::exists(f.destination("tree/value")) && read(f.destination("held/old")) == "existing"
    );
    fs::remove_all(f.destination("tree"));
    fs::rename(f.destination("held"), f.destination("tree"));
    write(f.destination("tree/value"), "old");
    pump.run(service, {location(f.source("tree"))}, f.destination(""), TransferAction::Copy, [&](const auto& c) {
      if (c.canMerge) {
        return TransferConflictChoice::Merge;
      }
      fs::rename(f.destination("tree"), f.destination("held"));
      fs::create_directory_symlink(f.root / "outside", f.destination("tree"));
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(!fs::exists(f.root / "outside/value") && read(f.destination("held/value")) == "old");
    TEST_CHECK(service.results().front().outcome != TransferOutcome::Copied);
    f.clean();
  }
  void testMergeLimitsAndNewEntries(Pump& pump) {
    Fixture f;
    files::TransferService service;
    fs::create_directory(f.source("tree"));
    fs::create_directory(f.destination("tree"));
    write(f.source("tree/a"), "captured");
    write(f.destination("tree/a"), "old");
    pump.run(service, {location(f.source("tree"))}, f.destination(""), TransferAction::Move, [&](const auto& c) {
      if (c.canMerge) {
        return TransferConflictChoice::Merge;
      }
      write(f.source("tree/new-arrival"), "not captured");
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(service.results().front().outcome == TransferOutcome::Partial);
    TEST_CHECK(read(f.source("tree/new-arrival")) == "not captured" && !fs::exists(f.destination("tree/new-arrival")));
    TEST_CHECK(read(f.destination("tree/a")) == "captured");
    for (unsigned i = 0; i < 16384; ++i) {
      write(f.source("tree/" + std::to_string(i)), "x");
    }
    pump.run(service, {location(f.source("tree"))}, f.destination(""), TransferAction::Copy, [](const auto&) {
      return TransferConflictChoice::Merge;
    });
    TEST_CHECK(service.results().front().outcome == TransferOutcome::Failed);
    TEST_CHECK(service.results().front().error.contains("16384") && !fs::exists(f.destination("tree/0")));
    f.clean();
  }
  void testRetry(Pump& pump) {
    if (::geteuid() == 0) {
      return; // Permission-denial fixture needs an ordinary process.
    }
    for (const auto action : {TransferAction::Copy, TransferAction::Move}) {
      Fixture f;
      files::TransferService service;
      fs::create_directories(f.source("tree"));
      fs::create_directories(f.destination("tree"));
      write(f.source("tree/a-done"), "completed");
      write(f.source("tree/z-failed"), "retry me");
      write(f.destination("tree/z-failed"), "existing");
      pump.run(service, {location(f.source("tree"))}, f.destination(""), action, [&](const auto& c) {
        if (c.canMerge) {
          return TransferConflictChoice::Merge;
        }
        TEST_CHECK(::chmod(f.destination("tree").c_str(), 0555) == 0);
        return TransferConflictChoice::Replace;
      });
      TEST_CHECK(service.results().front().outcome == TransferOutcome::Partial && service.retryable() == 1);
      struct stat before{}, after{};
      TEST_CHECK(::stat(f.destination("tree/a-done").c_str(), &before) == 0);
      TEST_CHECK(::chmod(f.destination("tree").c_str(), 0755) == 0);
      TEST_CHECK(service.retryFailed());
      finish(pump, service, [](const auto&) { return TransferConflictChoice::Replace; });
      TEST_CHECK(
          service.results().front().outcome
          == (action == TransferAction::Copy ? TransferOutcome::Copied : TransferOutcome::Moved)
      );
      TEST_CHECK(read(f.destination("tree/z-failed")) == "retry me" && service.retryable() == 0);
      TEST_CHECK(::stat(f.destination("tree/a-done").c_str(), &after) == 0 && before.st_ino == after.st_ino);
      TEST_CHECK(fs::exists(f.source("tree")) == (action == TransferAction::Copy));
      f.clean();
    }
    {
      Fixture f;
      files::TransferService service;
      fs::create_directories(f.source("tree"));
      fs::create_directories(f.destination("tree"));
      write(f.source("tree/a-done"), "completed");
      write(f.source("tree/z-failed"), "pending");
      write(f.destination("tree/z-failed"), "old");
      pump.run(service, {location(f.source("tree"))}, f.destination(""), TransferAction::Move, [&](const auto& c) {
        if (c.canMerge) {
          return TransferConflictChoice::Merge;
        }
        TEST_CHECK(::chmod(f.destination("tree").c_str(), 0555) == 0);
        return TransferConflictChoice::Replace;
      });
      TEST_CHECK(service.retryable() == 1);
      TEST_CHECK(service.retryFailed());
      service.cancel();
      pump.until([&] { return !service.busy(); });
      TEST_CHECK(service.results().front().outcome == TransferOutcome::Partial);
      TEST_CHECK(read(f.destination("tree/a-done")) == "completed" && !fs::exists(f.source("tree/a-done")));
      TEST_CHECK(read(f.source("tree/z-failed")) == "pending" && read(f.destination("tree/z-failed")) == "old");
      TEST_CHECK(::chmod(f.destination("tree").c_str(), 0755) == 0);
      f.clean();
    }
    // A failed standalone item retries without replaying a completed sibling.
    Fixture f;
    files::TransferService service;
    write(f.source("a"), "done");
    write(f.source("b"), "retry");
    write(f.destination("b"), "old");
    pump.run(
        service, {location(f.source("a")), location(f.source("b"))}, f.destination(""), TransferAction::Copy,
        [&](const auto&) {
          TEST_CHECK(::chmod(f.destination("").c_str(), 0555) == 0);
          return TransferConflictChoice::Replace;
        }
    );
    TEST_CHECK(service.retryable() == 1);
    TEST_CHECK(::chmod(f.destination("").c_str(), 0755) == 0);
    write(f.root / "new-b", "foreign source");
    fs::rename(f.root / "new-b", f.source("b"));
    TEST_CHECK(service.retryFailed());
    finish(pump, service, [](const auto&) { return TransferConflictChoice::Replace; });
    TEST_CHECK(service.retryable() == 0 && read(f.destination("b")) == "old" && read(f.destination("a")) == "done");
    TEST_CHECK(
        service.results().front().outcome == TransferOutcome::Copied
        && service.results().back().outcome == TransferOutcome::Failed
    );
    f.clean();
  }
} // namespace

int main() {
  try {
    Pump pump;
    testMoveAndCopy(pump);
    testConflicts(pump);
    testGuardsAndRaces(pump);
    testCancellationAndNumbering(pump);
    testCrossFilesystem(pump);
    testMerge(pump);
    testMergeSkipAndCancel(pump);
    testScopedChoices(pump);
    testMergeBindings(pump);
    testRetry(pump);
    testMergeLimitsAndNewEntries(pump);
    std::println("Transfer checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "Transfer test: {}", error.what());
    return 1;
  }
}
