#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "operations/copy_receipt.h"
#include "operations/move_receipt.h"
#include "operations/provider_transfer.h"
#include "test_check.h"

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <print>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  using namespace luciole;
  namespace fs = std::filesystem;
  using enum TransferOutcome;
  using enum TransferConflictChoice;
  void write(const fs::path& path, const std::string& data) { std::ofstream(path) << data; }
  std::string read(const fs::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  void wait(TransferService& service, const ProviderTransfer::Choose& choose = {}) {
    const auto deadline = g_get_monotonic_time() + 20000000;
    while (service.busy()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      if (const auto conflict = service.conflict()) {
        TEST_CHECK(choose);
        TEST_CHECK(service.resolve(conflict->id, choose(*conflict)));
      }
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
  }
  void drain(bool errors = false) {
    const auto deadline = g_get_monotonic_time() + 10000000;
    while (CopyReceipt::cleanupPending()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
    const auto messages = CopyReceipt::takeCleanupErrors();
    for (const auto& error : messages) {
      std::println(stderr, "cleanup: {}", error.message);
    }
    TEST_CHECK(messages.empty() != errors);
  }
  struct Result {
    std::vector<TransferResult> outcomes;
    std::shared_ptr<const MoveReceipt> receipt;
    bool retired = false;
  };
  Result move(
      const std::vector<Location>& sources, const Location& target,
      const ProviderTransfer::Choose& choose = [](const auto&) { return KeepBoth; }
  ) {
    const TransferRequest request{sources, target, TransferAction::Move};
    if (target.isNative()) {
      ProviderTransfer provider(request);
      std::atomic<std::uint64_t> bytes{0};
      std::atomic<std::size_t> completed{0};
      auto results = provider.run({}, bytes, completed, choose, nullptr, true);
      return {std::move(results), provider.takeMoveReceipt({})};
    }
    TransferService service;
    TEST_CHECK(service.start(request));
    wait(service, choose);
    return {service.results(), service.takeReceipt()};
  }
  Result replay(const std::shared_ptr<const MoveReceipt>& receipt) {
    TEST_CHECK(receipt);
    TransferService service;
    TEST_CHECK(service.replay(receipt));
    wait(service);
    return {service.results(), service.takeReceipt(), service.historyRetired()};
  }
  std::shared_ptr<const MoveReceipt> reversed(const std::shared_ptr<const MoveReceipt>& receipt) {
    auto result = replay(receipt);
    for (const auto& outcome : result.outcomes) {
      if (outcome.error) {
        std::println(stderr, "replay: {}", outcome.error->message);
      }
      TEST_CHECK(outcome.outcome == Moved && !outcome.error);
    }
    TEST_CHECK(result.receipt && !result.retired);
    return result.receipt;
  }
  void noStages(const fs::path& root) {
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
      TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-"));
    }
  }
  fs::path history(const fs::path& parent) {
    for (const auto& item : fs::directory_iterator(parent)) {
      if (item.path().filename().native().starts_with(".luciole-history-")) {
        return item.path();
      }
    }
    TEST_CHECK(false);
    return {};
  }
  void mount(const Location& root) {
    const GioPtr<GMountOperation> operation(g_mount_operation_new());
    g_signal_connect(
        operation.get(), "ask-question",
        G_CALLBACK(+[](GMountOperation* op, const char*, const char* const*, gpointer) {
          g_mount_operation_set_choice(op, 0);
          g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
        }),
        nullptr
    );
    g_signal_connect(
        operation.get(), "ask-password",
        G_CALLBACK(+[](GMountOperation* op, const char*, const char*, const char*, GAskPasswordFlags flags, gpointer) {
          if ((flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED) != 0) {
            g_mount_operation_set_anonymous(op, true);
            g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
          } else {
            g_mount_operation_reply(op, G_MOUNT_OPERATION_ABORTED);
          }
        }),
        nullptr
    );
    std::optional<bool> result;
    g_file_mount_enclosing_volume(
        gioFile(root).get(), G_MOUNT_MOUNT_NONE, operation.get(), nullptr,
        [](GObject* file, GAsyncResult* reply, gpointer data) {
          GError* error = nullptr;
          const bool ok = g_file_mount_enclosing_volume_finish(G_FILE(file), reply, &error);
          *static_cast<std::optional<bool>*>(data) =
              ok || (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED));
          if (!ok && error) {
            std::println(stderr, "mount: {}", error->message);
          }
          g_clear_error(&error);
        },
        &result
    );
    const auto deadline = g_get_monotonic_time() + 15000000;
    while (!result) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
    TEST_CHECK(*result);
  }
  void basic(const Location& remote, const fs::path& backend) {
    fs::create_directories(backend / "source/tree/nested");
    fs::create_directories(backend / "destination");
    const auto source = remote.child("source"), target = remote.child("destination");
    const std::string raw = std::string("raw-") + static_cast<char>(0xff);
    write(backend / "source/tree/nested/café %.txt", "nested contents");
    write(backend / "source/tree/.hidden", "hidden contents");
    fs::create_directory(backend / "source/tree/empty");
    fs::create_symlink("missing", backend / "source/tree/dangling");
    write(backend / "source" / raw, "raw bytes");
    {
      auto done = move({source.child("tree"), source.child(raw)}, target);
      for (const auto& item : done.outcomes) {
        if (item.error) {
          std::println(stderr, "move: {}", item.error->message);
        }
        TEST_CHECK(item.outcome == Moved && !item.error);
      }
      TEST_CHECK(done.receipt && done.receipt->size() == 2);
      TEST_CHECK(!fs::exists(backend / "source/tree") && !fs::exists(backend / "source" / raw));
      TEST_CHECK(read(backend / "destination/tree/nested/café %.txt") == "nested contents");
      write(backend / "source/later", "unrelated source sibling");
      write(backend / "destination/later", "unrelated destination sibling");
      auto redo = reversed(done.receipt);
      TEST_CHECK(read(backend / "source/tree/nested/café %.txt") == "nested contents");
      TEST_CHECK(fs::read_symlink(backend / "source/tree/dangling") == "missing");
      TEST_CHECK(!fs::exists(backend / "destination/tree"));
      auto stale = replay(done.receipt);
      TEST_CHECK(!stale.receipt && stale.retired);
      auto undo = reversed(redo);
      redo = reversed(undo);
      undo = reversed(redo);
      TEST_CHECK(read(backend / "destination" / raw) == "raw bytes");
      TEST_CHECK(read(backend / "source/later") == "unrelated source sibling");
      TEST_CHECK(read(backend / "destination/later") == "unrelated destination sibling");
    }
    drain();
    noStages(backend);
  }
  void replacements(const Location& remote, const fs::path& backend) {
    for (int shape = 0; shape < 3; ++shape) {
      const auto path = backend / std::to_string(shape);
      const auto root = remote.child(std::to_string(shape));
      fs::create_directories(path / "source");
      fs::create_directories(path / "destination");
      if (shape == 0) {
        write(path / "source/item", "incoming");
      } else {
        fs::create_directory(path / "source/item");
        write(path / "source/item/new", "incoming");
      }
      if (shape == 1) {
        write(path / "destination/item", "previous");
      } else {
        fs::create_directory(path / "destination/item");
        write(path / "destination/item/old", "previous");
      }
      {
        auto done =
            move({root.child("source").child("item")}, root.child("destination"), [](const auto&) { return Replace; });
        TEST_CHECK(done.receipt && done.outcomes.front().outcome == Moved);
        const auto retained = history(path / "destination");
        TEST_CHECK(
            shape == 1 ? read(retained / "previous") == "previous" : read(retained / "previous/old") == "previous"
        );
        struct stat mode{};
        TEST_CHECK(::stat(retained.c_str(), &mode) == 0 && (mode.st_mode & 0777U) == 0700);
        auto redo = reversed(done.receipt);
        TEST_CHECK(
            shape == 0 ? read(path / "source/item") == "incoming" : read(path / "source/item/new") == "incoming"
        );
        TEST_CHECK(
            shape == 1 ? read(path / "destination/item") == "previous"
                       : read(path / "destination/item/old") == "previous"
        );
        auto undo = reversed(redo);
        redo = reversed(undo);
      }
      drain();
      noStages(path);
    }
  }
  void collisions(const Location& remote, const fs::path& backend) {
    fs::create_directories(backend / "source");
    fs::create_directories(backend / "destination");
    write(backend / "source/a", "incoming");
    {
      auto done = move({remote.child("source").child("a")}, remote.child("destination"));
      TEST_CHECK(done.receipt);
      CancelToken token;
      token.cancel();
      std::atomic<std::size_t> completed{0};
      auto cancelled = done.receipt->reverse(token, completed);
      TEST_CHECK(!cancelled.retireHistory && cancelled.results.front().outcome == Cancelled);
      TEST_CHECK(!fs::exists(backend / "source/a") && read(backend / "destination/a") == "incoming");
      write(backend / "source/a", "later occupant");
      auto refused = replay(done.receipt);
      TEST_CHECK(!refused.receipt && !refused.retired && refused.outcomes.front().error->kind == FileErrorKind::Exists);
      TEST_CHECK(read(backend / "source/a") == "later occupant");
      fs::remove(backend / "source/a");
      auto redo = reversed(done.receipt);
      write(backend / "destination/a", "redo occupant");
      refused = replay(redo);
      TEST_CHECK(!refused.receipt && !refused.retired && read(backend / "destination/a") == "redo occupant");
      fs::remove(backend / "destination/a");
      auto undo = reversed(redo);
    }
    drain();
    noStages(backend);
  }
  void guards(const Location& remote, const fs::path& backend) {
    for (int variant = 0; variant < 4; ++variant) {
      const auto root = remote.child(std::to_string(variant));
      const auto path = backend / std::to_string(variant);
      fs::create_directories(path / "source");
      fs::create_directories(path / "destination");
      write(path / "source/a", "incoming");
      write(path / "destination/a", "previous");
      {
        auto done =
            move({root.child("source").child("a")}, root.child("destination"), [](const auto&) { return Replace; });
        TEST_CHECK(done.receipt);
        if (variant == 0) {
          struct stat before{};
          TEST_CHECK(::stat((path / "destination/a").c_str(), &before) == 0);
          write(path / "destination/a", "changed!");
          const timespec times[]{before.st_atim, before.st_mtim};
          TEST_CHECK(::utimensat(AT_FDCWD, (path / "destination/a").c_str(), times, 0) == 0);
        } else if (variant == 1) {
          fs::rename(path / "source", path / "held-source");
          fs::create_directory(path / "source");
        } else if (variant == 2) {
          write(history(path / "source") / "unknown", "preserve unknown");
        } else {
          write(history(path / "destination") / "previous", "edited previous");
        }
        const auto retained = history(path / "destination");
        auto refused = replay(done.receipt);
        TEST_CHECK(!refused.receipt && refused.retired && refused.outcomes.front().outcome == Failed);
        TEST_CHECK(!fs::exists(path / "source/a") && fs::exists(path / "destination/a"));
        TEST_CHECK(read(retained / "previous") == (variant == 3 ? "edited previous" : "previous"));
      }
      drain(true);
      TEST_CHECK(fs::exists(history(path / "destination") / "previous"));
      fs::remove_all(path);
    }
  }
  void preflight(const Location& remote, const fs::path& backend) {
    fs::create_directories(backend / "source");
    fs::create_directories(backend / "destination");
    write(backend / "source/a", "first");
    write(backend / "source/b", "second");
    {
      auto done =
          move({remote.child("source").child("a"), remote.child("source").child("b")}, remote.child("destination"));
      TEST_CHECK(done.receipt && done.receipt->size() == 2);
      write(backend / "destination/b", "edited");
      auto refused = replay(done.receipt);
      TEST_CHECK(!refused.receipt && refused.retired && read(backend / "destination/a") == "first");
      TEST_CHECK(!fs::exists(backend / "source/a") && !fs::exists(backend / "source/b"));
    }
    drain(true);
    fs::remove_all(backend);
  }
  void excluded(const Location& remote, const fs::path& backend) {
    fs::create_directories(backend / "source/tree");
    fs::create_directories(backend / "destination/tree");
    write(backend / "source/tree/a", "merged");
    write(backend / "destination/tree/keep", "unrelated");
    auto merged =
        move({remote.child("source").child("tree")}, remote.child("destination"), [](const auto&) { return Merge; });
    TEST_CHECK(!merged.receipt && merged.outcomes.front().outcome == Moved);
    TEST_CHECK(
        read(backend / "destination/tree/a") == "merged" && read(backend / "destination/tree/keep") == "unrelated"
    );
    drain();
    noStages(backend);
    write(backend / "source/a", "incoming");
    write(backend / "destination/a", "existing");
    auto skip =
        move({remote.child("source").child("a")}, remote.child("destination"), [](const auto&) { return Skip; });
    TEST_CHECK(!skip.receipt && skip.outcomes.front().outcome == Skipped);
    {
      auto numbered = move({remote.child("source").child("a")}, remote.child("destination"));
      TEST_CHECK(
          numbered.receipt && numbered.outcomes.front().destination == remote.child("destination").child("a (1)")
      );
      auto redo = reversed(numbered.receipt);
    }
    drain();
    noStages(backend);
    auto partial =
        move({remote.child("source").child("a"), remote.child("source").child("missing")}, remote.child("destination"));
    TEST_CHECK(
        !partial.receipt && partial.outcomes.front().outcome == Moved && partial.outcomes.back().outcome == Failed
    );
    drain();
    noStages(backend);
  }
  void overlapsAndLimits(const Location& remote, const fs::path& backend) {
    fs::create_directories(backend / "source/parent/child");
    fs::create_directories(backend / "destination");
    write(backend / "source/parent/child/file", "overlapping selection");
    auto result = move(
        {remote.child("source").child("parent"), remote.child("source").child("parent").child("child")},
        remote.child("destination")
    );
    TEST_CHECK(!result.receipt && result.outcomes[0].outcome == Moved && result.outcomes[1].outcome == Skipped);
    TEST_CHECK(read(backend / "destination/parent/child/file") == "overlapping selection");
    drain();
    noStages(backend);
    std::vector<Location> sources;
    sources.reserve(129);
    for (unsigned i = 0; i < 129; ++i) {
      const auto name = std::to_string(i);
      write(backend / "source" / name, "small file");
      sources.push_back(remote.child("source").child(name));
    }
    result = move(sources, remote.child("destination"));
    TEST_CHECK(!result.receipt);
    for (const auto& item : result.outcomes) {
      TEST_CHECK(item.outcome == Moved && !item.error);
    }
    drain();
    noStages(backend);
    fs::create_directory(backend / "source/special");
    TEST_CHECK(::mkfifo((backend / "source/special/fifo").c_str(), 0600) == 0);
    write(backend / "destination/special", "existing destination");
    result = move({remote.child("source").child("special")}, remote.child("destination"), [](const auto&) {
      return Replace;
    });
    TEST_CHECK(!result.receipt && result.outcomes.front().outcome == Failed);
    TEST_CHECK(
        fs::is_fifo(backend / "source/special/fifo") && read(backend / "destination/special") == "existing destination"
    );
    drain();
    noStages(backend);
  }
  void lostReply(const Location& remote, const fs::path& backend, const fs::path& arm, unsigned variant) {
    TEST_CHECK(variant < 4);
    fs::create_directories(backend / "source");
    fs::create_directories(backend / "destination");
    write(backend / "source/file", "incoming");
    if (variant != 0) {
      write(backend / "destination/file", "previous");
    }
    {
      auto moved = move({remote.child("source").child("file")}, remote.child("destination"), [](const auto&) {
        return Replace;
      });
      TEST_CHECK(moved.receipt && moved.outcomes.front().outcome == Moved);
      const auto retained = history(backend / "destination");
      auto receipt = variant >= 2 ? reversed(moved.receipt) : moved.receipt;
      const auto trigger = variant == 1 ? retained / "previous"
          : variant == 3                ? backend / "source/file"
                                        : backend / "destination/file";
      write(arm, trigger.native());
      auto uncertain = replay(receipt);
      TEST_CHECK(!uncertain.receipt && uncertain.retired && uncertain.outcomes.front().outcome == Partial);
      TEST_CHECK(uncertain.outcomes.front().error->message.contains("Inspect"));
      if (variant < 2) {
        TEST_CHECK(read(backend / "source/file") == "incoming");
        TEST_CHECK(
            variant == 0 ? !fs::exists(backend / "destination/file") : read(backend / "destination/file") == "previous"
        );
      } else {
        TEST_CHECK(read(retained / "previous") == "previous");
        TEST_CHECK(
            variant == 2 ? read(backend / "source/file") == "incoming"
                         : read(backend / "destination/file") == "incoming"
        );
      }
    }
    drain(true);
    TEST_CHECK(fs::exists(history(backend / "source") / "history-token"));
    TEST_CHECK(fs::exists(history(backend / "destination") / "history-token"));
  }
  void run(const Location& remote, const fs::path& backend) {
    basic(remote.child("basic"), backend / "basic");
    replacements(remote.child("replacements"), backend / "replacements");
    collisions(remote.child("collisions"), backend / "collisions");
    guards(remote.child("guards"), backend / "guards");
    preflight(remote.child("preflight"), backend / "preflight");
    excluded(remote.child("excluded"), backend / "excluded");
    overlapsAndLimits(remote.child("limits"), backend / "limits");
  }
} // namespace
int main(int argc, char* argv[]) {
  GlibPollSource glib;
  if (argc == 6 && std::string_view(argv[1]) == "--lost-reply") {
    const auto remote = Location::fromUri(argv[2]);
    TEST_CHECK(remote.uri().starts_with("sftp:"));
    mount(remote);
    const luciole::GString uuid(g_uuid_string_random());
    const auto name = std::string("lost-move-") + uuid.get();
    lostReply(remote.child(name), fs::path(argv[3]) / name, argv[4], static_cast<unsigned>(std::stoul(argv[5])));
  } else if (argc == 4 && std::string_view(argv[1]) == "--provider") {
    const auto remote = Location::fromUri(argv[2]);
    TEST_CHECK(remote.uri().starts_with("sftp:"));
    mount(remote);
    const luciole::GString uuid(g_uuid_string_random());
    const auto name = std::string("move-history-") + uuid.get();
    const auto backend = fs::path(argv[3]) / name;
    fs::create_directories(backend);
    run(remote.child(name), backend);
  } else {
    TEST_CHECK(argc == 1);
    const luciole::GString tmp(g_dir_make_tmp("luciole-provider-moves-XXXXXX", nullptr));
    TEST_CHECK(tmp);
    const fs::path backend(tmp.get());
    run(Location::fromPath(backend.native()), backend);
    fs::remove_all(backend);
  }
  std::println("Provider Move history checks passed");
}
