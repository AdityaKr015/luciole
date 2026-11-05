#include "app/glib_poll_source.h"
#include "operations/empty_trash_service.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using Outcome = files::EmptyTrashOutcome;
  using Status = files::EmptyTrashStatus;
  void require(bool value, const std::string& message) {
    if (!value) {
      throw std::runtime_error(message);
    }
  }
  void write(const fs::path& path, const std::string& value) { std::ofstream(path) << value; }
  std::string read(const fs::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  struct Fixture {
    fs::path root;
    Fixture() {
      const files::GString path(g_dir_make_tmp("noctalia-empty-trash-XXXXXX", nullptr));
      require(path != nullptr, "fixture failed");
      root = path.get();
    }
    ~Fixture() {
      std::error_code error;
      fs::remove_all(root, error);
    }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      while (!done()) {
        require(std::chrono::steady_clock::now() < deadline, "empty trash timed out");
        std::vector<pollfd> fds;
        const auto start = source.addPollFds(fds);
        require(::poll(fds.data(), fds.size(), 1) >= 0, "poll failed");
        source.dispatch(fds, start);
      }
    }
    void prepare(files::EmptyTrashService& service) {
      require(service.prepare(), "prepare refused");
      until([&] { return !service.busy(); });
      require(
          service.snapshot().status == Status::READY,
          service.snapshot().error.empty() ? "prepare did not become ready" : service.snapshot().error
      );
    }
    void empty(files::EmptyTrashService& service) {
      require(service.empty(), "empty refused");
      until([&] { return !service.busy(); });
      require(service.snapshot().status == Status::FINISHED, "actual outcomes lost");
    }

  private:
    GlibPollSource source;
  };
  void trash(const fs::path& path) {
    GError* error = nullptr;
    const bool ok = g_file_trash(files::Location::fromPath(path.native()).file().get(), nullptr, &error);
    const std::string message = error ? error->message : "trash failed";
    g_clear_error(&error);
    require(ok, message);
  }
  fs::path trashFiles() { return fs::path(g_get_user_data_dir()) / "Trash/files"; }
  fs::path trashInfo() { return fs::path(g_get_user_data_dir()) / "Trash/info"; }
  void unavailableAndLifetime(Pump& pump) {
    Fixture fixture;
    write(fixture.root / "keep", "native content");
    files::EmptyTrashService service;
    require(!service.empty(), "unprepared empty accepted");
    require(service.prepare(), "preparation refused");
    require(!service.prepare() && !service.empty(), "busy request accepted");
    pump.until([&] { return !service.busy(); });
    require(
        service.snapshot().status == Status::ERROR
            && !service.canEmpty()
            && read(fixture.root / "keep") == "native content",
        "missing provider deleted native data or claimed success"
    );
    require(service.prepare(), "cancel preparation refused");
    service.cancel();
    pump.until([&] { return !service.busy(); });
    require(service.snapshot().status == Status::CANCELLED && !service.empty(), "cancelled preparation committed");
    auto destroyed = std::make_unique<files::EmptyTrashService>();
    bool notified = false;
    destroyed->setChangedCallback([&] { notified = true; });
    require(destroyed->prepare(), "lifetime preparation refused");
    notified = false;
    destroyed.reset();
    require(service.prepare(), "drain preparation refused");
    pump.until([&] { return !service.busy(); });
    require(!notified, "destroyed observer called");
  }
  void successfulEmptyAndNewArrivals(Pump& pump) {
    Fixture fixture;
    files::EmptyTrashService service;
    pump.prepare(service);
    require(
        service.snapshot().results.empty() && !service.canEmpty() && !service.empty(), "empty trash enabled deletion"
    );
    fs::create_directories(fixture.root / "first/Folder 文");
    fs::create_directories(fixture.root / "second");
    write(fixture.root / "first/same #%.txt", "one");
    write(fixture.root / "second/same #%.txt", "two");
    write(fixture.root / "first/Folder 文/child", "nested");
    write(fixture.root / "outside", "untouched");
    fs::create_symlink(fixture.root / "outside", fixture.root / "link");
    fs::create_symlink("missing", fixture.root / "broken");
    fs::create_directory_symlink(fixture.root / "second", fixture.root / "first/Folder 文/external");
    const auto raw = std::string("raw-") + static_cast<char>(0xff);
    write(fixture.root / raw, "raw");
    write(fixture.root / ".hidden", "hidden");
    for (const auto& path :
         {fixture.root / "first/same #%.txt", fixture.root / "second/same #%.txt", fixture.root / "first/Folder 文",
          fixture.root / "link", fixture.root / "broken", fixture.root / raw, fixture.root / ".hidden"}) {
      trash(path);
    }
    pump.prepare(service);
    require(service.snapshot().results.size() == 7 && service.canEmpty(), "preparation omitted trash entries");
    const auto expectedSize =
        28U + (fixture.root / "outside").native().size() + (fixture.root / "second").native().size();
    require(
        service.snapshot().size == expectedSize && !service.snapshot().sizeIncomplete,
        "recursive size followed links, counted directory metadata or omitted file contents"
    );
    require(
        std::distance(fs::directory_iterator(trashInfo()), fs::directory_iterator{}) == 7,
        "prepare deleted recovery metadata"
    );
    service.cancel();
    require(!service.canEmpty() && fs::exists(trashFiles() / "same #%.txt"), "dialog cancellation deleted contents");
    pump.prepare(service);
    write(fixture.root / "arrived-later", "retain");
    trash(fixture.root / "arrived-later");
    pump.empty(service);
    require(
        service.snapshot().results.size() == 7
            && service.completed() == 7
            && std::ranges::all_of(
                service.snapshot().results, [](const auto& item) { return item.outcome == Outcome::DELETED; }
            ),
        "provider did not accept every prepared deletion"
    );
    require(
        read(fixture.root / "outside") == "untouched" && fs::is_directory(fixture.root / "second"),
        "symlink target followed"
    );
    require(
        fs::exists(trashFiles() / "arrived-later")
            && fs::exists(trashInfo() / "arrived-later.trashinfo")
            && std::distance(fs::directory_iterator(trashInfo()), fs::directory_iterator{}) == 1,
        "new arrivals deleted or metadata left behind"
    );
    pump.prepare(service);
    pump.empty(service);
    pump.prepare(service);
    require(
        service.snapshot().results.empty() && fs::is_empty(trashInfo()) && fs::is_empty(trashFiles()),
        "trash root or recovery metadata not empty"
    );
    service.cancel();
  }
  void staleItemsAndPartialOutcomes(Pump& pump) {
    Fixture fixture;
    write(fixture.root / "replaced", "original");
    write(fixture.root / "good", "remove");
    write(fixture.root / "restored", "restore");
    for (const auto* name : {"replaced", "good", "restored"}) {
      trash(fixture.root / name);
    }
    files::EmptyTrashService service;
    pump.prepare(service);
    fs::rename(trashFiles() / "replaced", fixture.root / "held-original");
    write(trashFiles() / "replaced", "replacement");
    fs::rename(trashFiles() / "restored", fixture.root / "restored");
    fs::remove(trashInfo() / "restored.trashinfo");
    pump.empty(service);
    require(
        std::ranges::count(service.snapshot().results, Outcome::DELETED, &files::EmptyTrashResult::outcome) == 1
            && std::ranges::count(service.snapshot().results, Outcome::FAILED, &files::EmptyTrashResult::outcome) == 1
            && std::ranges::count(service.snapshot().results, Outcome::SKIPPED, &files::EmptyTrashResult::outcome) == 1,
        "stale sources lost partial outcomes"
    );
    require(
        read(trashFiles() / "replaced") == "replacement"
            && read(fixture.root / "held-original") == "original"
            && read(fixture.root / "restored") == "restore",
        "replaced or restored item deleted"
    );
    pump.prepare(service);
    pump.empty(service);
  }
  void cancellationBetweenItems(Pump& pump) {
    Fixture fixture;
    for (int index = 0; index < 128; ++index) {
      const auto path = fixture.root / ("cancel-" + std::to_string(index));
      write(path, "content");
      trash(path);
    }
    files::EmptyTrashService service;
    pump.prepare(service);
    require(service.empty(), "cancellation job refused");
    pump.until([&] { return service.completed() != 0 || !service.busy(); });
    service.cancel();
    pump.until([&] { return !service.busy(); });
    require(service.snapshot().status == Status::FINISHED, "cancellation erased completed outcomes");
    for (const auto& result : service.snapshot().results) {
      const files::GString name(g_file_get_basename(result.location.file().get()));
      const bool exists = fs::exists(trashFiles() / name.get());
      require((result.outcome == Outcome::DELETED) == !exists, "cancellation result contradicted disk");
    }
    service.cancel();
    require(service.snapshot().status == Status::FINISHED, "late cancel replaced actual results");
    pump.prepare(service);
    if (service.canEmpty()) {
      pump.empty(service);
    } else {
      service.cancel();
    }
  }
  void confirmationDoesNotWaitForSize(Pump& pump) {
    Fixture fixture;
    fs::create_directory(fixture.root / "large-folder");
    for (int index = 0; index < 256; ++index) {
      write(fixture.root / "large-folder" / std::to_string(index), "data");
    }
    trash(fixture.root / "large-folder");
    files::EmptyTrashService service;
    bool requested = false, accepted = false, measuring = false;
    service.setChangedCallback([&] {
      if (!requested && service.snapshot().status == Status::READY) {
        requested = true;
        measuring = service.busy() && !service.snapshot().size;
        accepted = service.empty();
      }
    });
    require(service.prepare(), "early confirmation preparation refused");
    pump.until([&] {
      return service.snapshot().status == Status::FINISHED || service.snapshot().status == Status::ERROR;
    });
    require(
        requested
            && measuring
            && accepted
            && service.snapshot().results.front().outcome == Outcome::DELETED
            && !fs::exists(trashFiles() / "large-folder"),
        "confirmation waited for size or lost deletion"
    );
    service.setChangedCallback({});
  }
  void incompleteSize(Pump& pump) {
    if (::geteuid() == 0) {
      return;
    }
    Fixture fixture;
    fs::create_directory(fixture.root / "unreadable");
    write(fixture.root / "unreadable/content", "retain");
    trash(fixture.root / "unreadable");
    fs::permissions(trashFiles() / "unreadable", fs::perms::none);
    files::EmptyTrashService service;
    pump.prepare(service);
    require(service.snapshot().sizeIncomplete, "unreadable contents presented an exact size");
    fs::permissions(trashFiles() / "unreadable", fs::perms::owner_all);
    pump.prepare(service);
    require(
        service.snapshot().size == 6 && !service.snapshot().sizeIncomplete, "size did not recover after access restored"
    );
    pump.empty(service);
  }
} // namespace

int main(int argc, char** argv) {
  try {
    Pump pump;
    if (argc == 2 && std::string_view(argv[1]) == "--provider") {
      successfulEmptyAndNewArrivals(pump);
      staleItemsAndPartialOutcomes(pump);
      cancellationBetweenItems(pump);
      confirmationDoesNotWaitForSize(pump);
      incompleteSize(pump);
    } else {
      require(argc == 1, "unexpected test arguments");
      unavailableAndLifetime(pump);
    }
    std::println("empty-trash-service checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "empty-trash-service: {}", error.what());
    return 1;
  }
}
