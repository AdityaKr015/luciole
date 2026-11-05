#include "app/glib_poll_source.h"
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
        const auto first = source.addPollFds(fds);
        TEST_CHECK(::poll(fds.data(), fds.size(), 1) >= 0);
        source.dispatch(fds, first);
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
          TEST_CHECK(!service.resolve(conflict->id + 100000, TransferConflictChoice::REPLACE));
          const auto decision = choose ? choose(*conflict) : TransferConflictChoice::CANCEL;
          TEST_CHECK(service.resolve(conflict->id, decision));
        }
        return !service.busy();
      });
    }

  private:
    GlibPollSource source;
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
        TransferAction::MOVE
    );
    TEST_CHECK(service.results().size() == 2 && std::ranges::all_of(service.results(), [](const auto& result) {
                 return result.outcome == TransferOutcome::MOVED && result.error.empty();
               }));
    TEST_CHECK(
        !fs::exists(fixture.source("file.txt"))
        && read(fixture.destination("file.txt")) == "payload"
        && read(fixture.destination("folder/sub/.hidden")) == "nested"
    );
    TEST_CHECK(
        fs::is_symlink(fixture.destination("folder/link")) && read(fixture.root / "outside") == "outside must survive"
    );
    pump.run(service, {location(fixture.destination("folder"))}, fixture.source(""), TransferAction::COPY);
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::COPIED
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
    pump.run(service, {location(fixture.source(raw))}, fixture.destination(""), TransferAction::COPY);
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::COPIED && read(fixture.destination(raw)) == "raw filename"
    );
    fixture.clean();
  }
  void testConflicts(Pump& pump) {
    Fixture fixture;
    files::TransferService service;
    write(fixture.source("image.png"), "incoming");
    write(fixture.destination("image.png"), "existing");
    pump.run(
        service, {location(fixture.source("image.png"))}, fixture.destination(""), TransferAction::COPY,
        [](const auto&) { return TransferConflictChoice::SKIP; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::SKIPPED && read(fixture.destination("image.png")) == "existing"
    );
    write(fixture.destination("image (1).png"), "occupied");
    pump.run(
        service, {location(fixture.source("image.png"))}, fixture.destination(""), TransferAction::COPY,
        [](const auto&) { return TransferConflictChoice::KeepBoth; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::COPIED
        && read(fixture.destination("image (2).png")) == "incoming"
        && read(fixture.destination("image.png")) == "existing"
    );
    pump.run(
        service, {location(fixture.source("image.png"))}, fixture.destination(""), TransferAction::MOVE,
        [](const auto&) { return TransferConflictChoice::REPLACE; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::MOVED
        && !fs::exists(fixture.source("image.png"))
        && read(fixture.destination("image.png")) == "incoming"
    );
    fs::create_directories(fixture.source("folder"));
    fs::create_directories(fixture.destination("folder"));
    write(fixture.source("folder/new"), "new");
    write(fixture.destination("folder/old"), "old");
    pump.run(
        service, {location(fixture.source("folder"))}, fixture.destination(""), TransferAction::COPY,
        [](const auto&) { return TransferConflictChoice::REPLACE; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::COPIED
        && read(fixture.destination("folder/new")) == "new"
        && !fs::exists(fixture.destination("folder/old"))
        && fs::exists(fixture.source("folder/new"))
    );
    fs::create_symlink(fixture.root / "protected", fixture.destination("link"));
    write(fixture.root / "protected", "protected");
    write(fixture.source("link"), "regular incoming");
    pump.run(
        service, {location(fixture.source("link"))}, fixture.destination(""), TransferAction::COPY,
        [](const auto&) { return TransferConflictChoice::REPLACE; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::COPIED
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
    pump.run(service, {location(fixture.source("tree"))}, fixture.root / "alias", TransferAction::COPY);
    TEST_CHECK(service.results()[0].outcome == TransferOutcome::FAILED && fs::is_empty(fixture.source("tree/child")));
    pump.run(
        service, {location(fixture.source("tree/file")), location(fixture.source("tree"))}, fixture.destination(""),
        TransferAction::MOVE
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::SKIPPED
        && service.results()[1].outcome == TransferOutcome::MOVED
        && read(fixture.destination("tree/file")) == "tree"
    );
    write(fixture.source("race.txt"), "incoming");
    write(fixture.destination("race.txt"), "initial destination");
    int decisions = 0;
    pump.run(
        service, {location(fixture.source("race.txt"))}, fixture.destination(""), TransferAction::COPY,
        [&](const auto&) {
          if (++decisions == 1) {
            write(fixture.root / "replacement", "changed destination");
            fs::rename(fixture.root / "replacement", fixture.destination("race.txt"));
            return TransferConflictChoice::REPLACE;
          }
          return TransferConflictChoice::SKIP;
        }
    );
    TEST_CHECK(
        decisions == 2
        && service.results()[0].outcome == TransferOutcome::SKIPPED
        && read(fixture.destination("race.txt")) == "changed destination"
    );
    pump.run(
        service, {location(fixture.source("race.txt"))}, fixture.destination(""), TransferAction::MOVE,
        [&](const auto&) {
          write(fixture.root / "new source", "replaced source");
          fs::rename(fixture.root / "new source", fixture.source("race.txt"));
          return TransferConflictChoice::KeepBoth;
        }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::FAILED
        && read(fixture.source("race.txt")) == "replaced source"
        && !fs::exists(fixture.destination("race (1).txt"))
    );
    TEST_CHECK(::mkfifo(fixture.source("pipe").c_str(), 0600) == 0);
    pump.run(
        service, {location(fixture.source("pipe")), location(fixture.source("race.txt"))}, fixture.destination(""),
        TransferAction::COPY, [](const auto&) { return TransferConflictChoice::SKIP; }
    );
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::FAILED
        && service.results()[1].outcome == TransferOutcome::SKIPPED
    );
    fixture.clean();
  }
  void testCancellationAndNumbering(Pump& pump) {
    Fixture fixture;
    files::TransferService service;
    write(fixture.source("same.txt"), "same");
    pump.run(service, {location(fixture.source("same.txt"))}, fixture.source(""), TransferAction::COPY);
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::COPIED && read(fixture.source("same (1).txt")) == "same"
    );
    const std::string longName(255, 'a');
    write(fixture.source(longName), "long");
    pump.run(service, {location(fixture.source(longName))}, fixture.source(""), TransferAction::COPY);
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::COPIED
        && read(fixture.source(std::string(251, 'a') + " (1)")) == "long"
    );
    write(fixture.destination("same.txt"), "existing");
    pump.run(
        service, {location(fixture.source("same.txt")), location(fixture.source("same (1).txt"))},
        fixture.destination(""), TransferAction::MOVE, [](const auto&) { return TransferConflictChoice::CANCEL; }
    );
    TEST_CHECK(
        std::ranges::all_of(
            service.results(), [](const auto& item) { return item.outcome == TransferOutcome::CANCELLED; }
        )
        && read(fixture.destination("same.txt")) == "existing"
    );
    write(fixture.source("large"), "");
    fs::resize_file(fixture.source("large"), 1024ULL * 1024 * 1024);
    TEST_CHECK(
        service.start({{location(fixture.source("large"))}, location(fixture.destination("")), TransferAction::COPY})
    );
    TEST_CHECK(!service.start({{location(fixture.source("same.txt"))}, location(fixture.destination(""))}));
    pump.until([&] {
      if (service.bytes() != 0) {
        service.cancel();
      }
      return !service.busy();
    });
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::CANCELLED
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
    pump.run(service, {location(source.source("tree"))}, target.destination(""), TransferAction::MOVE);
    TEST_CHECK(
        service.results()[0].outcome == TransferOutcome::MOVED
        && !fs::exists(source.source("tree"))
        && read(target.destination("tree/sub/linked")) == "cross filesystem"
        && fs::is_symlink(target.destination("tree/broken"))
    );
    source.clean();
    target.clean();
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
    std::println("Transfer checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "Transfer test: {}", error.what());
    return 1;
  }
}
