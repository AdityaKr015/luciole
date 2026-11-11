#include "event_loop/glib_poll_source.h"
#include "operations/copy_receipt.h"
#include "operations/move_receipt.h"
#include "operations/transfer_service.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <thread>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using luciole::TransferAction;
  using luciole::TransferConflictChoice;
  using luciole::TransferOutcome;
  void write(const fs::path& path, const std::string& text) { std::ofstream(path) << text; }
  std::string read(const fs::path& path) {
    std::ifstream input(path);
    return {(std::istreambuf_iterator<char>(input)), {}};
  }
  luciole::Location location(const fs::path& path) { return luciole::Location::fromPath(path.native()); }
  struct Fixture {
    fs::path root;
    explicit Fixture(const std::string& parent = "/tmp") {
      std::string name = parent + "/luciole-transfer-XXXXXX";
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
        TEST_CHECK(!item.path().filename().native().starts_with(".luciole-transfer-"));
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
    run(luciole::TransferService& service, std::vector<luciole::Location> sources, const fs::path& destination,
        TransferAction action, std::function<TransferConflictChoice(const luciole::TransferConflict&)> choose = {}) {
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
    void replay(luciole::TransferService& service, const std::shared_ptr<const luciole::MoveReceipt>& receipt) {
      TEST_CHECK(receipt && service.replay(receipt));
      until([&] { return !service.busy(); });
    }
    void replayCopy(luciole::TransferService& service, const std::shared_ptr<const luciole::CopyReceipt>& receipt) {
      TEST_CHECK(receipt && service.replayCopy(receipt));
      until([&] { return !service.busy(); });
    }
    void cleanup(luciole::TransferService& service) {
      service.takeCopyReceipt().reset();
      until([] { return !luciole::CopyReceipt::cleanupPending(); });
    }

  private:
    GlibPollSource m_source;
  };
  void testCopyHistory(Pump& pump) {
    for (const auto action : {TransferAction::Copy, TransferAction::Link}) {
      Fixture f;
      luciole::TransferService service;
      const auto raw = std::string("raw-") + static_cast<char>(0xff);
      write(f.source("文.txt"), "original");
      fs::create_directories(f.source("folder/sub"));
      write(f.source("folder/sub/.inside"), "nested");
      fs::create_symlink("missing", f.source("broken"));
      write(f.source(raw), "raw");
      pump.run(
          service,
          {location(f.source("文.txt")), location(f.source("folder")), location(f.source("broken")),
           location(f.source(raw))},
          f.destination(""), action
      );
      TEST_CHECK(service.copyReceipt() && !service.receipt());
      const auto original = fs::status(f.destination("文.txt")).permissions();
      write(f.source("文.txt"), "source changed");
      auto stale = service.copyReceipt();
      std::shared_ptr<const luciole::CopyReceipt> staleHidden;
      for (int cycle = 0; cycle < 3; ++cycle) {
        pump.replayCopy(service, service.copyReceipt());
        TEST_CHECK(service.copyReceipt() && !service.historyRetired() && service.completed() == 4);
        TEST_CHECK(!fs::exists(f.destination("文.txt")) && !fs::exists(f.destination("folder")));
        TEST_CHECK(read(f.source("文.txt")) == "source changed" && read(f.source("folder/sub/.inside")) == "nested");
        luciole::TransferService other;
        pump.replayCopy(other, staleHidden ? staleHidden : stale);
        TEST_CHECK(other.historyRetired() && !other.copyReceipt());
        if (!staleHidden) {
          staleHidden = service.copyReceipt();
        }
        TEST_CHECK(
            service.results()[0].destination
            && fs::is_directory(fs::path(*service.results()[0].destination->nativePath()).parent_path())
        );
        pump.replayCopy(service, service.copyReceipt());
        TEST_CHECK(service.copyReceipt() && !service.historyRetired());
        if (action == TransferAction::Copy) {
          TEST_CHECK(
              read(f.destination("文.txt")) == "original" && read(f.destination("folder/sub/.inside")) == "nested"
          );
          TEST_CHECK(fs::read_symlink(f.destination("broken")) == "missing" && read(f.destination(raw)) == "raw");
        } else {
          TEST_CHECK(fs::read_symlink(f.destination("文.txt")) == f.source("文.txt"));
          TEST_CHECK(fs::read_symlink(f.destination("folder")) == f.source("folder"));
          TEST_CHECK(fs::read_symlink(f.destination("broken")) == f.source("broken"));
        }
        TEST_CHECK(fs::status(f.destination("文.txt")).permissions() == original);
      }
      TEST_CHECK(!service.retryFailed() && !service.replayCopy({}));
      pump.replayCopy(service, service.copyReceipt());
      stale.reset();
      staleHidden.reset();
      pump.cleanup(service);
      TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
      for (const auto& item : fs::directory_iterator(f.destination(""))) {
        TEST_CHECK(!item.path().filename().native().starts_with(".luciole-create-"));
      }
    }
  }
  void testCopyHistoryGuards(Pump& pump) {
    for (int scenario = 0; scenario < 7; ++scenario) {
      Fixture f;
      luciole::TransferService service;
      write(f.source("a"), "A");
      fs::create_directories(f.source("tree/sub"));
      write(f.source("tree/sub/inside"), "original");
      pump.run(service, {location(f.source("a")), location(f.source("tree"))}, f.destination(""), TransferAction::Copy);
      auto receipt = service.copyReceipt();
      TEST_CHECK(receipt);
      fs::path retained;
      if (scenario >= 5) {
        pump.replayCopy(service, receipt);
        receipt = service.copyReceipt();
        retained = *service.results()[1].destination->nativePath();
        if (scenario == 5) {
          write(retained / "sub/inside", "modified retained data");
        } else {
          fs::create_symlink("missing", f.destination("tree"));
        }
      } else if (scenario <= 1) {
        const auto path = scenario == 0 ? f.destination("a") : f.destination("tree/sub/inside");
        struct stat stamp{};
        TEST_CHECK(::stat(path.c_str(), &stamp) == 0);
        write(path, scenario == 0 ? "B" : "modified");
        const struct timespec times[]{stamp.st_atim, stamp.st_mtim};
        TEST_CHECK(::utimensat(AT_FDCWD, path.c_str(), times, 0) == 0);
      } else if (scenario == 2) {
        write(f.destination("tree/sub/new"), "new data");
      } else if (scenario == 3) {
        fs::rename(f.destination("tree"), f.destination("held"));
        fs::create_directory(f.destination("tree"));
      } else {
        fs::rename(f.root / "destination", f.root / "held-parent");
        fs::create_directory(f.root / "destination");
      }
      pump.replayCopy(service, receipt);
      TEST_CHECK(!service.copyReceipt() && service.results().size() == 2);
      TEST_CHECK(service.historyRetired() == (scenario != 6));
      TEST_CHECK(std::ranges::all_of(service.results(), [](const auto& result) {
        return result.outcome == TransferOutcome::Failed;
      }));
      if (scenario == 6) {
        TEST_CHECK(!fs::exists(f.destination("a")) && fs::is_symlink(f.destination("tree")));
        TEST_CHECK(fs::remove(f.destination("tree")));
        pump.replayCopy(service, receipt);
        TEST_CHECK(service.copyReceipt() && read(f.destination("a")) == "A");
      } else if (scenario >= 5) {
        TEST_CHECK(!fs::exists(f.destination("a")) && read(retained / "sub/inside") == "modified retained data");
      } else {
        const auto actual = scenario == 4 ? f.root / "held-parent/a" : f.destination("a");
        TEST_CHECK(read(actual) == (scenario == 0 ? "B" : "A"));
      }
      receipt.reset();
      pump.cleanup(service);
      const auto errors = luciole::CopyReceipt::takeCleanupErrors();
      TEST_CHECK(errors.empty() == (scenario != 5));
      if (scenario == 5) {
        TEST_CHECK(
            read(retained / "sub/inside") == "modified retained data"
            && std::ranges::any_of(errors, [&](const auto& error) { return error.message.contains(retained.native()); })
        );
      }
    }
  }
  void testCopyHistoryRetention(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    write(f.source("a.txt"), "incoming");
    write(f.destination("a.txt"), "existing");
    pump.run(
        service, {location(f.source("a.txt")), location(f.source("a.txt"))}, f.destination(""), TransferAction::Copy,
        [](const auto&) { return TransferConflictChoice::KeepBoth; }
    );
    TEST_CHECK(service.copyReceipt() && read(f.destination("a (1).txt")) == "incoming");
    TEST_CHECK(service.results()[1].outcome == TransferOutcome::Skipped);
    pump.replayCopy(service, service.copyReceipt());
    const fs::path retained = *service.results()[0].destination->nativePath();
    TEST_CHECK(read(retained) == "incoming" && read(f.destination("a.txt")) == "existing");
    pump.replayCopy(service, service.copyReceipt());
    TEST_CHECK(read(f.destination("a (1).txt")) == "incoming" && read(f.destination("a.txt")) == "existing");
    pump.replayCopy(service, service.copyReceipt());
    pump.cleanup(service);
    TEST_CHECK(!fs::exists(retained.parent_path()) && read(f.destination("a.txt")) == "existing");
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());

    fs::path deep = f.source("deep");
    for (int i = 0; i < 66; ++i) {
      deep /= "child";
    }
    fs::create_directories(deep);
    pump.run(service, {location(f.source("deep"))}, f.destination(""), TransferAction::Copy);
    TEST_CHECK(service.results()[0].outcome == TransferOutcome::Copied && !service.copyReceipt());
    pump.cleanup(service);
  }
  void testCopyHistoryCancellation(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    std::vector<luciole::Location> sources;
    for (int i = 0; i < 128; ++i) {
      write(f.source(std::to_string(i)), std::to_string(i));
      sources.push_back(location(f.source(std::to_string(i))));
    }
    pump.run(service, sources, f.destination(""), TransferAction::Copy);
    auto receipt = service.copyReceipt();
    TEST_CHECK(receipt);
    luciole::CancelToken token;
    token.cancel();
    std::atomic<std::size_t> completed{0};
    auto result = receipt->reverse(token, completed);
    TEST_CHECK(!result.retireHistory && !result.receipt && completed.load() == 128);
    TEST_CHECK(std::ranges::all_of(result.results, [](const auto& item) {
      return item.outcome == TransferOutcome::Cancelled;
    }));
    token = luciole::CancelToken{};
    completed.store(0);
    std::jthread interrupt([&](const std::stop_token& stop) {
      while (!stop.stop_requested() && completed.load() == 0) {
        std::this_thread::yield();
      }
      token.cancel();
    });
    result = receipt->reverse(token, completed);
    interrupt.request_stop();
    interrupt.join();
    TEST_CHECK(completed.load() == 128);
    for (std::size_t i = 0; i < result.results.size(); ++i) {
      const auto& item = result.results[i];
      if (item.outcome == TransferOutcome::Moved) {
        TEST_CHECK(item.destination && read(*item.destination->nativePath()) == std::to_string(i));
        TEST_CHECK(!fs::exists(f.destination(std::to_string(i))));
      } else {
        TEST_CHECK(
            item.outcome == TransferOutcome::Cancelled && read(f.destination(std::to_string(i))) == std::to_string(i)
        );
      }
      TEST_CHECK(read(f.source(std::to_string(i))) == std::to_string(i));
    }
    result.receipt.reset();
    receipt.reset();
    pump.cleanup(service);
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty() == !result.retireHistory);
  }

  void testMoveHistory(Pump& pump) {
    Fixture f;
    luciole::TransferService service;
    const auto raw = std::string("raw-") + static_cast<char>(0xff);
    const std::vector<std::string> names{"file 文.txt", "folder", "broken", raw};
    write(f.source(names[0]), "original");
    fs::create_directories(f.source("folder/sub"));
    write(f.source("folder/sub/inside"), "nested");
    fs::create_symlink("absent", f.source("broken"));
    write(f.source(raw), "raw");
    std::vector<luciole::Location> sources;
    sources.reserve(names.size());
    for (const auto& name : names) {
      sources.push_back(location(f.source(name)));
    }
    pump.run(service, sources, f.root / "destination", TransferAction::Move);
    TEST_CHECK(service.receipt() && !service.historyRetired());
    write(f.destination("folder/sub/inside"), "edited nested");
    for (int cycle = 0; cycle < 3; ++cycle) {
      pump.replay(service, service.receipt());
      TEST_CHECK(service.receipt() && !service.historyRetired() && service.completed() == names.size());
      TEST_CHECK(read(f.source(names[0])) == "original" && read(f.source(raw)) == "raw");
      TEST_CHECK(
          read(f.source("folder/sub/inside")) == "edited nested" && fs::read_symlink(f.source("broken")) == "absent"
      );
      TEST_CHECK(fs::is_empty(f.root / "destination"));
      pump.replay(service, service.receipt());
      TEST_CHECK(service.receipt() && !service.historyRetired());
      TEST_CHECK(read(f.destination(names[0])) == "original" && read(f.destination(raw)) == "raw");
      TEST_CHECK(read(f.destination("folder/sub/inside")) == "edited nested");
      TEST_CHECK(fs::read_symlink(f.destination("broken")) == "absent" && fs::is_empty(f.root / "source"));
    }
    TEST_CHECK(!service.retryFailed() && !service.replay({}));
    f.clean();
  }

  void testMoveHistoryGuards(Pump& pump) {
    for (int scenario = 0; scenario < 5; ++scenario) {
      Fixture f;
      luciole::TransferService service;
      write(f.source("a"), "A");
      write(f.source("b"), "B");
      pump.run(
          service, {location(f.source("a")), location(f.source("b"))}, f.root / "destination", TransferAction::Move
      );
      const auto receipt = service.receipt();
      TEST_CHECK(receipt);
      if (scenario == 0) {
        fs::create_symlink("missing", f.source("b"));
      } else if (scenario == 1) {
        struct stat stamp{};
        TEST_CHECK(::stat(f.destination("b").c_str(), &stamp) == 0);
        write(f.destination("b"), "X");
        const struct timespec times[]{stamp.st_atim, stamp.st_mtim};
        TEST_CHECK(::utimensat(AT_FDCWD, f.destination("b").c_str(), times, 0) == 0);
      } else if (scenario == 2) {
        fs::rename(f.destination("b"), f.destination("held"));
        write(f.destination("b"), "foreign");
      } else if (scenario == 3) {
        fs::rename(f.root / "source", f.root / "held-source");
        fs::create_directory(f.root / "source");
      } else {
        fs::rename(f.root / "destination", f.root / "held-destination");
        fs::create_directory(f.root / "destination");
      }
      pump.replay(service, receipt);
      TEST_CHECK(!service.receipt() && service.results().size() == 2);
      TEST_CHECK(std::ranges::all_of(service.results(), [](const auto& result) {
        return result.outcome == TransferOutcome::Failed && !result.retryable;
      }));
      TEST_CHECK(service.historyRetired() == (scenario != 0));
      TEST_CHECK(!fs::exists(f.source("a")));
      TEST_CHECK(read((scenario == 4 ? f.root / "held-destination/a" : f.destination("a"))) == "A");
      if (scenario == 0) {
        TEST_CHECK(fs::read_symlink(f.source("b")) == "missing");
        TEST_CHECK(fs::remove(f.source("b")));
        pump.replay(service, receipt);
        TEST_CHECK(service.receipt() && read(f.source("a")) == "A" && read(f.source("b")) == "B");
      } else if (scenario == 1) {
        TEST_CHECK(read(f.destination("b")) == "X");
      } else if (scenario == 2) {
        TEST_CHECK(read(f.destination("b")) == "foreign" && read(f.destination("held")) == "B");
      }
    }
  }

  void testMoveHistoryAliasesAndNames(Pump& pump) {
    Fixture f;
    write(f.source("a"), "alias");
    fs::create_hard_link(f.source("a"), f.source("b"));
    write(f.destination("a"), "existing");
    luciole::TransferService service;
    pump.run(
        service, {location(f.source("a")), location(f.source("b"))}, f.root / "destination", TransferAction::Move,
        [](const auto&) { return TransferConflictChoice::KeepBoth; }
    );
    TEST_CHECK(service.receipt() && fs::equivalent(f.destination("a (1)"), f.destination("b")));
    for (int cycle = 0; cycle < 3; ++cycle) {
      pump.replay(service, service.receipt());
      TEST_CHECK(service.receipt() && fs::equivalent(f.source("a"), f.source("b")));
      TEST_CHECK(read(f.destination("a")) == "existing");
      pump.replay(service, service.receipt());
      TEST_CHECK(service.receipt() && fs::equivalent(f.destination("a (1)"), f.destination("b")));
    }
    pump.replay(service, service.receipt());
    pump.run(
        service, {location(f.source("a")), location(f.source("a"))}, f.root / "destination", TransferAction::Move,
        [](const auto&) { return TransferConflictChoice::KeepBoth; }
    );
    TEST_CHECK(service.receipt() && service.results()[1].outcome == TransferOutcome::Skipped);
    pump.replay(service, service.receipt());
    TEST_CHECK(service.receipt() && read(f.source("a")) == "alias");
  }

  void testMoveHistoryInterruption(Pump& pump) {
    for (const bool collision : {false, true}) {
      Fixture f;
      luciole::TransferService service;
      constexpr std::size_t count = 256;
      std::vector<luciole::Location> sources;
      for (std::size_t i = 0; i < count; ++i) {
        write(f.source(std::to_string(i)), std::to_string(i));
        sources.push_back(location(f.source(std::to_string(i))));
      }
      pump.run(service, sources, f.destination(""), TransferAction::Move);
      const auto receipt = service.receipt();
      TEST_CHECK(receipt);
      luciole::CancelToken token;
      token.cancel();
      std::atomic<std::size_t> completed{0};
      const auto cancelled = receipt->reverse(token, completed);
      TEST_CHECK(!cancelled.receipt && !cancelled.retireHistory && completed.load() == count);
      TEST_CHECK(std::ranges::all_of(cancelled.results, [](const auto& result) {
        return result.outcome == TransferOutcome::Cancelled;
      }));
      token = luciole::CancelToken{};
      completed.store(0);
      std::jthread interrupt([&](const std::stop_token& stop) {
        while (!stop.stop_requested() && completed.load() == 0) {
          std::this_thread::yield();
        }
        if (!stop.stop_requested() && completed.load() < count) {
          if (collision) {
            const int occupied =
                ::open(f.source(std::to_string(count - 1)).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (occupied >= 0) {
              TEST_CHECK(::write(occupied, "occupied", 8) == 8);
              TEST_CHECK(::close(occupied) == 0);
            } else {
              TEST_CHECK(errno == EEXIST);
            }
          } else {
            token.cancel();
          }
        }
      });
      const auto result = receipt->reverse(token, completed);
      interrupt.request_stop();
      interrupt.join();
      TEST_CHECK(completed.load() == count && result.results.size() == count);
      std::size_t moved = 0;
      for (std::size_t i = 0; i < count; ++i) {
        const auto& item = result.results[i];
        const auto name = std::to_string(i);
        if (item.outcome == TransferOutcome::Moved) {
          ++moved;
          TEST_CHECK(read(f.source(name)) == name && !fs::exists(f.destination(name)));
        } else {
          TEST_CHECK(read(f.destination(name)) == name);
          TEST_CHECK(
              !fs::exists(f.source(name)) || (collision && i == count - 1 && read(f.source(name)) == "occupied")
          );
          TEST_CHECK(item.error);
        }
      }
      TEST_CHECK(moved > 0);
      TEST_CHECK(moved == count ? result.receipt != nullptr : result.retireHistory && !result.receipt);
      f.clean();
    }
  }

  void testTransferHistoryBounds(Pump& pump) {
    Fixture f;
    std::vector<luciole::Location> sources;
    for (int i = 0; i < 100; ++i) {
      const auto name = std::to_string(i);
      write(f.source(name), name);
      sources.push_back(location(f.source(name)));
    }
    struct LimitGuard {
      struct rlimit previous{};
      LimitGuard() {
        TEST_CHECK(::getrlimit(RLIMIT_NOFILE, &previous) == 0);
        const struct rlimit limited{std::min<rlim_t>(previous.rlim_cur, 192), previous.rlim_max};
        TEST_CHECK(::setrlimit(RLIMIT_NOFILE, &limited) == 0);
      }
      ~LimitGuard() { TEST_CHECK(::setrlimit(RLIMIT_NOFILE, &previous) == 0); }
    } guard;
    luciole::TransferService service;
    pump.run(service, sources, f.root / "destination", TransferAction::Copy);
    TEST_CHECK(!service.copyReceipt());
    TEST_CHECK(std::ranges::all_of(service.results(), [](const auto& result) {
      return result.outcome == TransferOutcome::Copied;
    }));
    for (int i = 0; i < 100; ++i) {
      TEST_CHECK(read(f.destination(std::to_string(i))) == std::to_string(i));
      TEST_CHECK(fs::remove(f.destination(std::to_string(i))));
    }
    pump.run(service, sources, f.root / "destination", TransferAction::Move);
    TEST_CHECK(!service.receipt());
    TEST_CHECK(std::ranges::all_of(service.results(), [](const auto& result) {
      return result.outcome == TransferOutcome::Moved;
    }));
    for (int i = 0; i < 100; ++i) {
      TEST_CHECK(read(f.destination(std::to_string(i))) == std::to_string(i));
    }
  }

  void testMoveAndCopy(Pump& pump) {
    Fixture fixture;
    luciole::TransferService service;
    write(fixture.source("file.txt"), "payload");
    fs::create_directories(fixture.source("folder/sub"));
    write(fixture.source("folder/sub/.hidden"), "nested");
    const bool attributes =
        ::setxattr(fixture.source("folder/sub/.hidden").c_str(), "user.luciole-test", "metadata", 8, 0) == 0;
    fs::create_symlink("../../outside", fixture.source("folder/link"));
    write(fixture.root / "outside", "outside must survive");
    pump.run(
        service, {location(fixture.source("file.txt")), location(fixture.source("folder"))}, fixture.destination(""),
        TransferAction::Move
    );
    TEST_CHECK(service.results().size() == 2 && std::ranges::all_of(service.results(), [](const auto& result) {
                 return result.outcome == TransferOutcome::Moved && !result.error;
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
    TEST_CHECK(!service.receipt());
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::Copied
        && read(fixture.source("folder/sub/.hidden")) == "nested"
        && fs::read_symlink(fixture.source("folder/link")) == "../../outside"
    );
    if (attributes) {
      char value[16]{};
      TEST_CHECK(
          ::getxattr(fixture.source("folder/sub/.hidden").c_str(), "user.luciole-test", value, sizeof(value)) == 8
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
    luciole::TransferService service;
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
    TEST_CHECK(!service.receipt());
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
    luciole::TransferService service;
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
    luciole::TransferService service;
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
    luciole::TransferService service;
    pump.run(service, {location(source.source("tree"))}, target.destination(""), TransferAction::Move);
    TEST_CHECK(!service.receipt());
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
      Pump& pump, luciole::TransferService& service,
      const std::function<TransferConflictChoice(const luciole::TransferConflict&)>& choose, bool all = false
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
      luciole::TransferService service;
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
      TEST_CHECK(!service.receipt());
      f.clean();
    }
  }
  void testMergeSkipAndCancel(Pump& pump) {
    for (const auto choice : {TransferConflictChoice::Skip, TransferConflictChoice::Cancel}) {
      Fixture f;
      luciole::TransferService service;
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
    luciole::TransferService service;
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
    luciole::TransferService service;
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
    luciole::TransferService service;
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
    TEST_CHECK(
        service.results().front().error
        && service.results().front().error->message.contains("16384")
        && !fs::exists(f.destination("tree/0"))
    );
    f.clean();
  }
  void testRetry(Pump& pump) {
    if (::geteuid() == 0) {
      return; // Permission-denial fixture needs an ordinary process.
    }
    for (const auto action : {TransferAction::Copy, TransferAction::Move}) {
      Fixture f;
      luciole::TransferService service;
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
      luciole::TransferService service;
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
    luciole::TransferService service;
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
  void testSymbolicLinkDrops(Pump& pump) {
    Fixture f;
    fs::create_directories(f.source("folder/nested"));
    write(f.source("folder/nested/child"), "inside");
    write(f.source("café.txt"), "source bytes");
    fs::create_symlink("missing", f.source("broken"));
    luciole::TransferService service;
    pump.run(
        service,
        {location(f.source("folder")), location(f.source("folder/nested/child")), location(f.source("café.txt")),
         location(f.source("broken"))},
        f.root / "destination", TransferAction::Link
    );
    TEST_CHECK(service.results().size() == 4 && service.bytes() == 0);
    for (const auto& result : service.results()) {
      TEST_CHECK(result.outcome == TransferOutcome::Linked && result.destination && !result.error);
      TEST_CHECK(fs::read_symlink(*result.destination->nativePath()) == *result.source.nativePath());
    }
    TEST_CHECK(read(f.source("café.txt")) == "source bytes" && read(f.destination("café.txt")) == "source bytes");
    TEST_CHECK(read(f.source("folder/nested/child")) == "inside" && fs::is_symlink(f.destination("folder")));
    TEST_CHECK(fs::is_symlink(f.source("broken")) && fs::is_symlink(f.destination("broken")));
    TEST_CHECK(service.report().contains("Linked"));
    // Collision decisions never merge directories for a symbolic-link operation.
    pump.run(
        service, {location(f.source("folder"))}, f.root / "destination", TransferAction::Link,
        [](const auto& conflict) {
          TEST_CHECK(!conflict.canMerge);
          return TransferConflictChoice::KeepBoth;
        }
    );
    TEST_CHECK(service.results().front().outcome == TransferOutcome::Linked);
    TEST_CHECK(fs::read_symlink(f.destination("folder (1)")) == f.source("folder"));
    fs::remove(f.destination("café.txt"));
    write(f.destination("café.txt"), "old destination");
    pump.run(service, {location(f.source("café.txt"))}, f.root / "destination", TransferAction::Link, [](const auto&) {
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(
        service.results().front().outcome == TransferOutcome::Linked && fs::is_symlink(f.destination("café.txt"))
    );
    TEST_CHECK(read(f.source("café.txt")) == "source bytes");
    pump.run(service, {location(f.source("café.txt"))}, f.root / "source", TransferAction::Link);
    TEST_CHECK(fs::is_symlink(f.source("café (1).txt")) && read(f.source("café.txt")) == "source bytes");
    // A captured source replaced while a collision is pending is never linked.
    TEST_CHECK(
        service.start({{location(f.source("café.txt"))}, location(f.root / "destination"), TransferAction::Link})
    );
    pump.until([&] { return service.conflict().has_value(); });
    fs::rename(f.source("café.txt"), f.source("original"));
    write(f.source("café.txt"), "replacement");
    TEST_CHECK(service.resolve(service.conflict()->id, TransferConflictChoice::KeepBoth));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.results().front().outcome == TransferOutcome::Failed);
    TEST_CHECK(!fs::exists(f.destination("café (1).txt")) && read(f.source("café.txt")) == "replacement");
    pump.run(service, {location(f.source("folder"))}, f.source("folder/nested"), TransferAction::Link);
    TEST_CHECK(
        service.results().front().outcome == TransferOutcome::Failed && !fs::exists(f.source("folder/nested/folder"))
    );
    f.clean();
  }

} // namespace

int main() {
  try {
    Pump pump;
    testSymbolicLinkDrops(pump);
    testCopyHistory(pump);
    testCopyHistoryGuards(pump);
    testCopyHistoryRetention(pump);
    testCopyHistoryCancellation(pump);
    testMoveHistory(pump);
    testMoveHistoryGuards(pump);
    testMoveHistoryAliasesAndNames(pump);
    testMoveHistoryInterruption(pump);
    testTransferHistoryBounds(pump);
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
    pump.until([] { return !luciole::CopyReceipt::cleanupPending(); });
    TEST_CHECK(luciole::CopyReceipt::takeCleanupErrors().empty());
    std::println("Transfer checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "Transfer test: {}", error.what());
    return 1;
  }
}
