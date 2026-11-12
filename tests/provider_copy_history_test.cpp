#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "operations/provider_copy_receipt.h"
#include "operations/provider_transfer.h"
#include "test_check.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  using namespace luciole;
  namespace fs = std::filesystem;
  using enum TransferConflictChoice;
  using enum TransferOutcome;
  void write(const fs::path& path, const std::string& bytes) { std::ofstream(path) << bytes; }
  std::string read(const fs::path& path) {
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), {}};
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
    TEST_CHECK(errors ? !messages.empty() : messages.empty());
  }
  struct Result {
    std::vector<TransferResult> outcomes;
    std::shared_ptr<const CopyReceipt> receipt;
  };
  Result upload(
      const std::vector<Location>& sources, const Location& destination,
      const ProviderTransfer::Choose& choose = [](const auto&) { return KeepBoth; }
  ) {
    const TransferRequest request{sources, destination, TransferAction::Copy};
    if (destination.isNative()) {
      ProviderTransfer provider(request);
      std::atomic<std::uint64_t> bytes{0};
      std::atomic<std::size_t> completed{0};
      auto results = provider.run({}, bytes, completed, choose, nullptr, true);
      return {std::move(results), provider.takeCopyReceipt({})};
    }
    TransferService service;
    TEST_CHECK(service.start(request));
    wait(service, choose);
    return {service.results(), service.takeCopyReceipt()};
  }
  Result replay(const std::shared_ptr<const CopyReceipt>& receipt) {
    TEST_CHECK(receipt);
    TransferService service;
    TEST_CHECK(service.replayCopy(receipt));
    wait(service);
    return {service.results(), service.takeCopyReceipt()};
  }
  std::shared_ptr<const CopyReceipt> reversed(const std::shared_ptr<const CopyReceipt>& receipt) {
    auto result = replay(receipt);
    for (const auto& outcome : result.outcomes) {
      if (outcome.error) {
        std::println(stderr, "replay: {}", outcome.error->message);
      }
      TEST_CHECK(outcome.outcome == Moved && !outcome.error);
    }
    TEST_CHECK(result.receipt);
    return result.receipt;
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
  void noStages(const fs::path& path) {
    for (const auto& entry : fs::recursive_directory_iterator(path)) {
      TEST_CHECK(!entry.path().filename().native().starts_with(".luciole-"));
    }
  }
  fs::path anchor(const fs::path& path) {
    for (const auto& entry : fs::directory_iterator(path)) {
      if (entry.path().filename().native().starts_with(".luciole-history-")) {
        return entry.path();
      }
    }
    TEST_CHECK(false);
    return {};
  }
  void basic(const fs::path& local, const Location& remote, const fs::path& backend) {
    fs::create_directories(local / "source/tree/nested");
    fs::create_directories(backend);
    write(local / "source/tree/nested/file.txt", "tree content");
    write(local / "source/tree/.hidden", "hidden content");
    fs::create_symlink("missing target", local / "source/tree/dangling");
    write(local / "source/café.txt", "file content");
    fs::create_directory(local / "source/tree/empty");
    const std::string raw = std::string("raw-") + static_cast<char>(0xff);
    write(local / "source" / raw, "raw content");
    const auto source = Location::fromPath((local / "source").native());
    {
      auto done = upload({source.child("tree"), source.child("café.txt"), source.child(raw)}, remote);
      for (const auto& value : done.outcomes) {
        if (value.error) {
          std::println(stderr, "upload: {}", value.error->message);
        }
        TEST_CHECK(value.outcome == Copied && !value.error);
      }
      TEST_CHECK(done.receipt && done.receipt->size() == 3);
      TEST_CHECK(read(backend / "tree/nested/file.txt") == "tree content");
      struct stat original{};
      TEST_CHECK(::lstat((backend / raw).c_str(), &original) == 0);
      fs::remove_all(local / "source");
      auto redo = reversed(done.receipt);
      TEST_CHECK(!fs::exists(backend / "tree") && !fs::exists(backend / "café.txt"));
      auto undo = reversed(redo);
      TEST_CHECK(read(backend / "tree/nested/file.txt") == "tree content");
      TEST_CHECK(fs::is_symlink(backend / "tree/dangling"));
      TEST_CHECK(fs::is_empty(backend / "tree/empty") && read(backend / raw) == "raw content");
      struct stat restored{};
      TEST_CHECK(::lstat((backend / raw).c_str(), &restored) == 0 && original.st_ino == restored.st_ino);
      const auto stale = replay(done.receipt);
      TEST_CHECK(!stale.receipt && stale.outcomes.front().error->kind == FileErrorKind::Changed);
      redo = reversed(undo);
      undo = reversed(redo);
    }
    drain();
    noStages(backend);
  }
  void collisionsAndEdits(const fs::path& local, const Location& remote, const fs::path& backend) {
    fs::create_directories(local / "source");
    fs::create_directories(backend);
    write(local / "source/file.txt", "original bytes");
    const auto source = Location::fromPath((local / "source/file.txt").native());
    {
      auto done = upload({source}, remote);
      TEST_CHECK(done.receipt);
      auto redo = reversed(done.receipt);
      write(backend / "file.txt", "unrelated occupant");
      auto blocked = replay(redo);
      TEST_CHECK(!blocked.receipt && blocked.outcomes.front().error->kind == FileErrorKind::Exists);
      TEST_CHECK(read(backend / "file.txt") == "unrelated occupant");
      fs::remove(backend / "file.txt");
      auto undo = reversed(redo);
      // Preserve the provider's ordinary metadata: the digest must still detect this edit.
      const auto time = fs::last_write_time(backend / "file.txt");
      write(backend / "file.txt", "modified bytes");
      fs::last_write_time(backend / "file.txt", time);
      blocked = replay(undo);
      TEST_CHECK(!blocked.receipt && blocked.outcomes.front().error->kind == FileErrorKind::Changed);
      TEST_CHECK(read(backend / "file.txt") == "modified bytes");
    }
    drain();
    noStages(backend);
    {
      auto numbered = upload({source}, remote);
      TEST_CHECK(numbered.receipt && read(backend / "file (1).txt") == "original bytes");
      auto redo = reversed(numbered.receipt);
      auto undo = reversed(redo);
    }
    drain();
    noStages(backend);
    {
      const auto result = upload({source}, remote, [](const auto&) { return Replace; });
      TEST_CHECK(result.outcomes.front().outcome == Copied && !result.receipt);
    }
    drain();
    noStages(backend);
  }
  void cancellationAndDiscard(const fs::path& local, const Location& remote, const fs::path& backend) {
    fs::create_directories(local / "source");
    fs::create_directories(backend);
    write(local / "source/item", "cancel content");
    const auto source = Location::fromPath((local / "source/item").native());
    {
      auto done = upload({source}, remote);
      TEST_CHECK(done.receipt);
      const CancelToken token;
      token.cancel();
      std::atomic<std::size_t> completed{0};
      const auto reply = done.receipt->reverse(token, completed);
      TEST_CHECK(!reply.receipt && !reply.retireHistory && reply.results.front().outcome == Cancelled);
      TEST_CHECK(read(backend / "item") == "cancel content");
      auto redo = reversed(done.receipt);
      TEST_CHECK(!fs::exists(backend / "item"));
    }
    drain();
    noStages(backend);
    TEST_CHECK(!fs::exists(backend / "item") && read(local / "source/item") == "cancel content");
  }
  void cleanupRefusal(const fs::path& local, const Location& remote, const fs::path& backend) {
    fs::create_directories(local / "source");
    fs::create_directories(backend);
    write(local / "source/item", "owned content");
    const auto source = Location::fromPath((local / "source/item").native());
    fs::path retained;
    {
      auto done = upload({source}, remote);
      TEST_CHECK(done.receipt);
      auto redo = reversed(done.receipt);
      retained = anchor(backend);
      write(retained / "item", "edited retained bytes");
    }
    drain(true);
    TEST_CHECK(read(retained / "item") == "edited retained bytes");
    fs::remove_all(retained);
    {
      auto done = upload({source}, remote);
      TEST_CHECK(done.receipt);
      retained = anchor(backend);
      fs::rename(backend, backend.string() + "-held");
      fs::create_directory(backend);
      const auto reply = replay(done.receipt);
      TEST_CHECK(!reply.receipt && reply.outcomes.front().error->kind == FileErrorKind::Missing);
      TEST_CHECK(fs::is_empty(backend));
    }
    drain(true);
    fs::remove(backend);
    fs::rename(backend.string() + "-held", backend);
    TEST_CHECK(read(backend / "item") == "owned content");
    fs::remove_all(retained);
  }
  void budget(const fs::path& local, const Location& remote, const fs::path& backend) {
    fs::create_directories(local);
    fs::create_directories(backend);
    write(local / "small", "bytes");
    provider_history::Budget none{.entries = 1, .bytes = 1};
    bool refused = false;
    try {
      (void)ProviderCopyEffect::prepare(
          remote.child("small"), Location::fromPath((local / "small").native()), none, {}
      );
    } catch (const FileErrorException& error) {
      refused = error.error().kind == FileErrorKind::Unsupported;
    }
    TEST_CHECK(refused);
    drain();
    provider_history::Budget ordinary;
    bool missing = false;
    try {
      (void)ProviderCopyEffect::prepare(
          remote.child("missing-parent/item"), Location::fromPath((local / "small").native()), ordinary, {}
      );
    } catch (const FileErrorException& error) {
      missing = error.error().kind == FileErrorKind::Missing;
    }
    TEST_CHECK(missing);
    drain();
    noStages(backend);
  }
  void fullSelection(const fs::path& local, const Location& remote, const fs::path& backend) {
    fs::create_directories(local);
    fs::create_directories(backend);
    write(local / "a", "first bytes");
    write(local / "b", "second bytes");
    {
      auto done =
          upload({Location::fromPath((local / "a").native()), Location::fromPath((local / "b").native())}, remote);
      TEST_CHECK(done.receipt && done.receipt->size() == 2);
      write(backend / "b", "external edit");
      const auto reply = replay(done.receipt);
      TEST_CHECK(!reply.receipt);
      TEST_CHECK(read(backend / "a") == "first bytes" && read(backend / "b") == "external edit");
    }
    drain();
    noStages(backend);
    fs::create_directories(local / "tree");
    fs::create_directories(backend / "tree");
    write(local / "tree/child", "merged");
    write(backend / "tree/keep", "kept");
    {
      auto done = upload({Location::fromPath((local / "tree").native())}, remote, [](const auto&) { return Merge; });
      TEST_CHECK(!done.receipt && done.outcomes.front().outcome == Copied);
      TEST_CHECK(read(backend / "tree/child") == "merged" && read(backend / "tree/keep") == "kept");
    }
    drain();
    noStages(backend);
    {
      auto done = upload({Location::fromPath((local / "a").native())}, remote, [](const auto&) { return Skip; });
      TEST_CHECK(!done.receipt && done.outcomes.front().outcome == Skipped);
    }
    drain();
    noStages(backend);
  }
  void markerRefusal(const fs::path& local, const Location& remote, const fs::path& backend) {
    fs::create_directories(local);
    fs::create_directories(backend);
    write(local / "item", "visible content");
    write(local / "outside", "outside canary");
    fs::path retained;
    {
      auto done = upload({Location::fromPath((local / "item").native())}, remote);
      TEST_CHECK(done.receipt);
      retained = anchor(backend);
      fs::create_symlink(local / "outside", retained / "unknown");
      const auto reply = replay(done.receipt);
      TEST_CHECK(!reply.receipt && reply.outcomes.front().error->kind == FileErrorKind::Changed);
      TEST_CHECK(read(backend / "item") == "visible content");
    }
    drain(true);
    TEST_CHECK(read(local / "outside") == "outside canary" && fs::is_symlink(retained / "unknown"));
    fs::remove_all(retained);
  }
  void effectLimit(const fs::path& local, const Location& remote, const fs::path& backend) {
    fs::create_directories(local);
    fs::create_directories(backend);
    std::vector<Location> sources;
    for (unsigned i = 0; i < 129; ++i) {
      const auto path = local / ("item-" + std::to_string(i));
      write(path, "bounded bytes");
      sources.push_back(Location::fromPath(path.native()));
    }
    {
      const auto result = upload(sources, remote);
      TEST_CHECK(!result.receipt && result.outcomes.size() == sources.size());
      for (const auto& value : result.outcomes) {
        TEST_CHECK(value.outcome == Copied && !value.error);
      }
    }
    drain();
    noStages(backend);
    for (unsigned i = 0; i < 129; ++i) {
      TEST_CHECK(read(backend / ("item-" + std::to_string(i))) == "bounded bytes");
    }
  }
  void unmount(const Location& remote) {
    GError* error = nullptr;
    const GioPtr<GMount> mounted(g_file_find_enclosing_mount(gioFile(remote).get(), nullptr, &error));
    TEST_CHECK(mounted && !error);
    std::optional<bool> done;
    g_mount_unmount_with_operation(
        mounted.get(), G_MOUNT_UNMOUNT_FORCE, nullptr, nullptr,
        [](GObject* object, GAsyncResult* reply, gpointer data) {
          *static_cast<std::optional<bool>*>(data) =
              g_mount_unmount_with_operation_finish(G_MOUNT(object), reply, nullptr);
        },
        &done
    );
    const auto deadline = g_get_monotonic_time() + 5000000;
    while (!done) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
    TEST_CHECK(*done);
  }
  void disconnected(const fs::path& local, const Location& remote, const fs::path& backend) {
    fs::create_directories(local);
    fs::create_directories(backend);
    write(local / "item", "reconnect content");
    {
      auto done = upload({Location::fromPath((local / "item").native())}, remote);
      TEST_CHECK(done.receipt);
      unmount(remote);
      TransferService service;
      TEST_CHECK(service.replayCopy(done.receipt));
      wait(service);
      TEST_CHECK(!service.copyReceipt() && !service.historyRetired());
      TEST_CHECK(service.results().front().error && read(backend / "item") == "reconnect content");
      mount(remote);
      auto redo = reversed(done.receipt);
      auto undo = reversed(redo);
    }
    drain();
    noStages(backend);
  }

  void lostReply(const fs::path& local, const Location& remote, const fs::path& backend, const fs::path& arm) {
    fs::create_directories(local);
    fs::create_directories(backend);
    write(local / "item", "survives lost rename reply");
    fs::path retained;
    {
      auto done = upload({Location::fromPath((local / "item").native())}, remote);
      TEST_CHECK(done.receipt);
      retained = anchor(backend);
      const auto temporary = fs::path(arm.string() + ".tmp");
      write(temporary, (backend / "item").native());
      fs::rename(temporary, arm);
      TransferService service;
      TEST_CHECK(service.replayCopy(done.receipt));
      wait(service);
      TEST_CHECK(service.historyRetired() && !service.copyReceipt());
      TEST_CHECK(service.results().front().outcome == Partial && service.results().front().error);
      TEST_CHECK(!fs::exists(backend / "item") && read(retained / "item") == "survives lost rename reply");
      TEST_CHECK(service.report().contains("Inspect") && service.report().contains(retained.filename().native()));
      fs::remove(arm);
    }
    drain(true);
    TEST_CHECK(read(retained / "item") == "survives lost rename reply");
    TEST_CHECK(read(local / "item") == "survives lost rename reply");
    fs::remove_all(retained);
  }

  void run(const fs::path& local, const Location& remote, const fs::path& backend) {
    basic(local / "basic", remote.child("basic"), backend / "basic");
    collisionsAndEdits(local / "collisions", remote.child("collisions"), backend / "collisions");
    cancellationAndDiscard(local / "cancel", remote.child("cancel"), backend / "cancel");
    cleanupRefusal(local / "refusal", remote.child("refusal"), backend / "refusal");
    budget(local / "budget", remote.child("budget"), backend / "budget");
    fullSelection(local / "selection", remote.child("selection"), backend / "selection");
    markerRefusal(local / "marker", remote.child("marker"), backend / "marker");
    if (remote.isNative()) {
      effectLimit(local / "limit", remote.child("limit"), backend / "limit");
    } else {
      disconnected(local / "disconnected", remote.child("disconnected"), backend / "disconnected");
    }
  }
} // namespace

int main(int argc, char* argv[]) {
  GlibPollSource glib;
  if (argc == 6 && std::string_view(argv[1]) == "--lost-reply") {
    const auto remote = Location::fromUri(argv[2]);
    TEST_CHECK(remote.uri().starts_with("sftp:"));
    mount(remote);
    const luciole::GString uuid(g_uuid_string_random());
    const auto name = std::string("lost-reply-") + uuid.get();
    const auto backend = fs::path(argv[3]) / name, local = fs::path(argv[4]) / name;
    lostReply(local, remote.child(name), backend, argv[5]);
  } else if (argc == 5 && std::string_view(argv[1]) == "--provider") {
    const auto remote = Location::fromUri(argv[2]);
    TEST_CHECK(remote.uri().starts_with("sftp:"));
    mount(remote);
    const luciole::GString uuid(g_uuid_string_random());
    const auto name = std::string("history-") + uuid.get();
    const auto backend = fs::path(argv[3]) / name, local = fs::path(argv[4]) / name;
    fs::create_directories(backend);
    fs::create_directories(local);
    run(local, remote.child(name), backend);
  } else {
    TEST_CHECK(argc == 1);
    const luciole::GString temp(g_dir_make_tmp("luciole-provider-history-XXXXXX", nullptr));
    TEST_CHECK(temp);
    const fs::path root(temp.get());
    fs::create_directories(root / "backend");
    run(root / "local", Location::fromPath((root / "backend").native()), root / "backend");
    fs::remove_all(root);
  }
  std::println("Provider copy history checks passed");
}
