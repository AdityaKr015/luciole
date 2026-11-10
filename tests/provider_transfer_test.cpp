#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "operations/provider_transfer.h"
#include "operations/transfer_service.h"
#include "test_check.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using namespace files;
  using enum TransferAction;
  using enum TransferConflictChoice;
  using enum TransferOutcome;
  void write(const fs::path& path, const std::string& text) { std::ofstream(path) << text; }
  std::string read(const fs::path& path) {
    std::ifstream file(path);
    return {(std::istreambuf_iterator<char>(file)), {}};
  }
  void
  wait(TransferService& service, const std::function<TransferConflictChoice(const TransferConflict&)>& choose = {}) {
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
  void mount(const Location& root) {
    // These CLI fixtures only target explicitly supplied private servers.
    const GioPtr<GMountOperation> operation(g_mount_operation_new());
    g_signal_connect(
        operation.get(), "ask-question",
        G_CALLBACK(+[](GMountOperation* operation, const char*, const char* const*, gpointer) {
          g_mount_operation_set_choice(operation, 0);
          g_mount_operation_reply(operation, G_MOUNT_OPERATION_HANDLED);
        }),
        nullptr
    );
    g_signal_connect(
        operation.get(), "ask-password",
        G_CALLBACK(+[](GMountOperation* operation, const char*, const char*, const char*, GAskPasswordFlags flags,
                       gpointer) {
          if ((flags & G_ASK_PASSWORD_ANONYMOUS_SUPPORTED) != 0) {
            g_mount_operation_set_anonymous(operation, true);
            g_mount_operation_reply(operation, G_MOUNT_OPERATION_HANDLED);
          } else {
            g_mount_operation_reply(operation, G_MOUNT_OPERATION_ABORTED);
          }
        }),
        nullptr
    );
    std::optional<bool> mounted;
    g_file_mount_enclosing_volume(
        gioFile(root).get(), G_MOUNT_MOUNT_NONE, operation.get(), nullptr,
        [](GObject* file, GAsyncResult* result, gpointer data) {
          GError* error = nullptr;
          const bool ok = g_file_mount_enclosing_volume_finish(G_FILE(file), result, &error);
          *static_cast<std::optional<bool>*>(data) =
              ok || (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED));
          if (error && !static_cast<std::optional<bool>*>(data)->value()) {
            std::println(stderr, "mount: {}", error->message);
          }
          g_clear_error(&error);
        },
        &mounted
    );
    const auto deadline = g_get_monotonic_time() + 15000000;
    while (!mounted) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
    TEST_CHECK(*mounted);
  }
  void disconnect(const Location& root) {
    GError* error = nullptr;
    const GioPtr<GMount> mounted(g_file_find_enclosing_mount(gioFile(root).get(), nullptr, &error));
    TEST_CHECK(mounted && !error);
    std::optional<bool> done;
    g_mount_unmount_with_operation(
        mounted.get(), G_MOUNT_UNMOUNT_FORCE, nullptr, nullptr,
        [](GObject* mount, GAsyncResult* result, gpointer data) {
          *static_cast<std::optional<bool>*>(data) =
              g_mount_unmount_with_operation_finish(G_MOUNT(mount), result, nullptr);
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
  void portable(
      const fs::path& root, const Location& remote, const fs::path& backend, const Location& readOnly,
      const fs::path& readOnlyBackend
  ) {
    fs::create_directories(root / "source/tree/nested");
    fs::create_directories(root / "destination");
    const auto source = Location::fromPath((root / "source").native()),
               local = Location::fromPath((root / "destination").native());
    write(root / "source/tree/nested/file.txt", "nested portable bytes");
    write(root / "source/tree/.hidden", "hidden portable bytes");
    write(root / "source/café space.txt", "unicode portable bytes");
    const auto transfer = [&](std::vector<Location> sources, const Location& destination, TransferAction action,
                              const std::function<TransferConflictChoice(const TransferConflict&)>& answer = {}) {
      TransferService service;
      TEST_CHECK(service.start({std::move(sources), destination, action}));
      wait(service, answer);
      for (const auto& result : service.results()) {
        if (result.error) {
          std::println(stderr, "result: {}", result.error->message);
        }
      }
      return service.results();
    };
    auto result = transfer({source.child("tree"), source.child("café space.txt")}, remote, TransferAction::Copy);
    TEST_CHECK(result.size() == 2 && result[0].outcome == Copied && result[1].outcome == Copied);
    TEST_CHECK(
        read(backend / "tree/nested/file.txt") == "nested portable bytes"
        && read(backend / "tree/.hidden") == "hidden portable bytes"
    );
    TEST_CHECK(read(backend / "café space.txt") == "unicode portable bytes");
    result = transfer({remote.child("tree")}, local, TransferAction::Move);
    TEST_CHECK(
        result.front().outcome == Moved
        && !fs::exists(backend / "tree")
        && read(root / "destination/tree/nested/file.txt") == "nested portable bytes"
    );
    fs::create_directories(backend / "tree/nested");
    write(backend / "tree/keep.txt", "destination-only");
    write(backend / "tree/nested/file.txt", "old");
    result = transfer({source.child("tree")}, remote, TransferAction::Copy, [](const auto& conflict) {
      return conflict.canMerge ? Merge : TransferConflictChoice::Replace;
    });
    TEST_CHECK(
        result.front().outcome == Copied
        && read(backend / "tree/keep.txt") == "destination-only"
        && read(backend / "tree/nested/file.txt") == "nested portable bytes"
    );
    result =
        transfer({source.child("café space.txt")}, remote, TransferAction::Copy, [](const auto&) { return KeepBoth; });
    TEST_CHECK(result.front().outcome == Copied && read(backend / "café space (1).txt") == "unicode portable bytes");
    result = transfer({source.child("café space.txt")}, remote, TransferAction::Move, [](const auto&) { return Skip; });
    TEST_CHECK(result.front().outcome == Skipped && fs::exists(root / "source/café space.txt"));
    // Source changes while a conflict is pending must not replace the destination.
    result = transfer({source.child("café space.txt")}, remote, TransferAction::Move, [&](const auto&) {
      write(root / "source/café space.txt", "changed while waiting with another length");
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(
        result.front().outcome == Failed
        && result.front().error->kind == FileErrorKind::Changed
        && read(backend / "café space.txt") == "unicode portable bytes"
    );
    // New names upload and move in the opposite direction as well.
    write(root / "source/upload.txt", "move upload");
    result = transfer({source.child("upload.txt")}, remote, TransferAction::Move);
    TEST_CHECK(
        result.front().outcome == Moved
        && !fs::exists(root / "source/upload.txt")
        && read(backend / "upload.txt") == "move upload"
    );
    // A protocol unable to represent local symlinks must preserve the whole source tree.
    fs::create_directory(root / "source/links");
    write(root / "outside.txt", "outside canary");
    fs::create_symlink(root / "outside.txt", root / "source/links/link");
    result = transfer({source.child("links")}, remote, TransferAction::Move);
    TEST_CHECK(
        result.front().outcome == Failed
        && fs::is_symlink(root / "source/links/link")
        && !fs::exists(backend / "links")
        && read(root / "outside.txt") == "outside canary"
    );
    const std::string raw = std::string("raw-") + static_cast<char>(0xff) + ".txt";
    write(root / "source" / raw, "raw original");
    result = transfer({source.child(raw)}, remote, TransferAction::Move);
    TEST_CHECK(
        result.front().outcome == Failed
        && result.front().error->kind == FileErrorKind::InvalidArgument
        && fs::exists(root / "source" / raw)
    );
    if (remote.uri().starts_with("smb:")) {
      for (const auto* name : {"notes:stream", "CON.txt", "trailing.", "bad?.txt"}) {
        write(root / "source" / name, "unsupported name");
        result = transfer({source.child(name)}, remote, TransferAction::Move);
        TEST_CHECK(
            result.front().outcome == Failed
            && result.front().error->kind == FileErrorKind::InvalidArgument
            && fs::exists(root / "source" / name)
        );
      }
      fs::create_directory(backend / "CaseFolder");
      write(backend / "CaseFolder/kept.txt", "case canary");
      result = transfer({remote.child("CaseFolder/kept.txt")}, remote.child("casefolder"), TransferAction::Move);
      TEST_CHECK(result.front().outcome == Skipped && read(backend / "CaseFolder/kept.txt") == "case canary");
      result = transfer({remote.child("CaseFolder")}, remote.child("casefolder"), TransferAction::Copy);
      TEST_CHECK(
          result.front().outcome == Failed
          && result.front().error->kind == FileErrorKind::WouldRecurse
          && read(backend / "CaseFolder/kept.txt") == "case canary"
      );
    }
    const auto readName = "readonly-" + *remote.basename() + ".txt";
    write(readOnlyBackend / readName, "readonly original");
    result = transfer({readOnly.child(readName)}, local, TransferAction::Copy);
    TEST_CHECK(result.front().outcome == Copied && read(root / "destination" / readName) == "readonly original");
    result = transfer({readOnly.child(readName)}, local, TransferAction::Move, [](const auto&) {
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(
        result.front().outcome == Partial
        && fs::exists(readOnlyBackend / readName)
        && read(root / "destination" / readName) == "readonly original"
    );
    result = transfer({source.child("café space.txt")}, readOnly, TransferAction::Copy);
    TEST_CHECK(result.front().outcome == Failed && !fs::exists(readOnlyBackend / "café space.txt"));
    // Cancellation while streaming leaves the original and no published partial file.
    {
      std::ofstream large(root / "source/large.bin");
      large.seekp(128 * 1024 * 1024 - 1);
      large.put('X');
    }
    TransferService streaming;
    TEST_CHECK(streaming.start({{source.child("large.bin")}, remote, TransferAction::Copy}));
    const auto deadline = g_get_monotonic_time() + 15000000;
    while (streaming.busy() && streaming.bytes() == 0) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      TEST_CHECK(g_get_monotonic_time() < deadline);
      g_usleep(1000);
    }
    TEST_CHECK(streaming.busy());
    streaming.cancel();
    wait(streaming);
    TEST_CHECK(
        streaming.results().front().outcome == Cancelled
        && !fs::exists(backend / "large.bin")
        && fs::exists(root / "source/large.bin")
    );
    fs::remove(root / "source/large.bin");
    for (const auto& item : fs::recursive_directory_iterator(backend)) {
      TEST_CHECK(!item.path().filename().native().starts_with(".noctalia-transfer-"));
    }
    write(root / "destination/upload.txt", "destination survives disconnect");
    TransferService disconnected;
    TEST_CHECK(disconnected.start({{remote.child("upload.txt")}, local, TransferAction::Move}));
    wait(disconnected, [&](const auto&) {
      disconnect(remote);
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(
        disconnected.results().front().outcome == Failed
        && read(root / "destination/upload.txt") == "destination survives disconnect"
        && read(backend / "upload.txt") == "move upload"
    );
  }
  void run(const fs::path& root, const Location& remote, const fs::path& backend, bool provider) {
    fs::create_directories(root / "source/tree/nested");
    fs::create_directories(root / "destination");
    write(root / "source/tree/nested/file.txt", "nested bytes");
    write(root / "source/tree/.hidden", "hidden bytes");
    fs::create_symlink("nested/file.txt", root / "source/tree/link");
    write(root / "source/café space.txt", "unicode bytes");
    const std::string raw = std::string("raw-") + static_cast<char>(0xff) + ".txt";
    write(root / "source" / raw, "raw bytes");
    const auto native = Location::fromPath((root / "source").native());
    const auto local = Location::fromPath((root / "destination").native());
    auto transfer = [&](std::vector<Location> sources, const Location& destination, TransferAction action,
                        TransferConflictChoice choice = TransferConflictChoice::Replace) {
      if (provider) {
        TransferService service;
        TEST_CHECK(service.start({std::move(sources), destination, action}));
        wait(service, [choice](const auto&) { return choice; });
        return service.results();
      }
      ProviderTransfer service({std::move(sources), destination, action});
      std::atomic<std::uint64_t> bytes = 0;
      std::atomic<std::size_t> count = 0;
      return service.run(CancelToken{}, bytes, count, [choice](const auto& conflict) {
        TEST_CHECK(conflict.sourceModified && conflict.destinationModified);
        return choice;
      });
    };
    auto results = transfer(
        {native.child("tree"), native.child("café space.txt"), native.child(raw)}, remote, TransferAction::Copy
    );
    TEST_CHECK(results.size() == 3);
    for (const auto& result : results) {
      if (result.error) {
        std::println(stderr, "copy: {}", result.error->message);
      }
      TEST_CHECK(result.outcome == Copied);
    }
    TEST_CHECK(
        read(backend / "tree/nested/file.txt") == "nested bytes" && read(backend / "tree/.hidden") == "hidden bytes"
    );
    TEST_CHECK(fs::is_symlink(backend / "tree/link") && fs::read_symlink(backend / "tree/link") == "nested/file.txt");
    TEST_CHECK(read(backend / "café space.txt") == "unicode bytes" && read(backend / raw) == "raw bytes");
    results = transfer({remote.child("tree")}, local, TransferAction::Move);
    TEST_CHECK(
        results.front().outcome == Moved
        && !fs::exists(backend / "tree")
        && read(root / "destination/tree/nested/file.txt") == "nested bytes"
    );
    // Folder merge keeps destination-only entries and commits child conflicts independently.
    fs::create_directories(backend / "tree/nested");
    write(backend / "tree/only-destination", "keep");
    write(backend / "tree/nested/file.txt", "old");
    if (provider) {
      TransferService service;
      TEST_CHECK(service.start({{native.child("tree")}, remote, TransferAction::Copy}));
      wait(service, [](const auto& conflict) { return conflict.canMerge ? Merge : TransferConflictChoice::Replace; });
      TEST_CHECK(service.results().front().outcome == Copied);
    } else {
      ProviderTransfer service({{native.child("tree")}, remote, TransferAction::Copy});
      std::atomic<std::uint64_t> bytes = 0;
      std::atomic<std::size_t> count = 0;
      TEST_CHECK(
          service
              .run(
                  CancelToken{}, bytes, count,
                  [](const auto& conflict) { return conflict.canMerge ? Merge : TransferConflictChoice::Replace; }
              )
              .front()
              .outcome
          == Copied
      );
    }
    TEST_CHECK(
        read(backend / "tree/only-destination") == "keep" && read(backend / "tree/nested/file.txt") == "nested bytes"
    );
    results = transfer({native.child("café space.txt")}, remote, TransferAction::Copy, KeepBoth);
    TEST_CHECK(results.front().outcome == Copied && read(backend / "café space (1).txt") == "unicode bytes");
    results = transfer({native.child(raw)}, remote, TransferAction::Move, Skip);
    TEST_CHECK(results.front().outcome == Skipped && fs::exists(root / "source" / raw));
    results = transfer({native.child("tree")}, native.child("tree/nested"), TransferAction::Copy);
    TEST_CHECK(results.front().outcome == Failed && results.front().error->kind == FileErrorKind::WouldRecurse);
    // Replacing the captured source while the conflict question is open must not publish it.
    const CancelToken token;
    ProviderTransfer changed({{native.child(raw)}, remote, TransferAction::Move});
    std::atomic<std::uint64_t> bytes = 0;
    std::atomic<std::size_t> count = 0;
    results = changed.run(token, bytes, count, [&](const auto&) {
      write(root / "source" / raw, "replacement source with different length");
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(
        results.front().outcome == Failed
        && results.front().error->kind == FileErrorKind::Changed
        && read(backend / raw) == "raw bytes"
    );
    ProviderTransfer cancelled({{native.child(raw)}, remote, TransferAction::Move});
    results = cancelled.run(token, bytes, count, [&](const auto&) {
      token.cancel();
      return Cancel;
    });
    TEST_CHECK(results.front().outcome == Cancelled && cancelled.retryable() == 0 && fs::exists(root / "source" / raw));
    // A write-denied copy can be explicitly retried after permissions are corrected.
    fs::create_directory(backend / "denied");
    fs::permissions(backend / "denied", fs::perms::owner_read | fs::perms::owner_exec);
    if (geteuid() != 0) {
      TransferService retry;
      TEST_CHECK(retry.start({{native.child("café space.txt")}, remote.child("denied"), TransferAction::Copy}));
      wait(retry);
      // The normal service routes all-local jobs through its native implementation too.
      TEST_CHECK(retry.results().front().outcome == Failed);
      fs::permissions(backend / "denied", fs::perms::owner_all);
      if (provider) {
        TEST_CHECK(retry.retryable() == 1 && retry.retryFailed());
        wait(retry);
        TEST_CHECK(retry.results().front().outcome == Copied);
      }
    }
    fs::permissions(backend / "denied", fs::perms::owner_all);
    if (provider) {
      write(root / "source/to-remote.txt", "move upload");
      results = transfer({native.child("to-remote.txt")}, remote, TransferAction::Move);
      TEST_CHECK(
          results.front().outcome == Moved
          && !fs::exists(root / "source/to-remote.txt")
          && read(backend / "to-remote.txt") == "move upload"
      );
      // A completed upload never permits a failed source deletion to masquerade as a successful move.
      fs::create_directory(backend / "read-source");
      write(backend / "read-source/stays.txt", "retain original");
      fs::permissions(backend / "read-source", fs::perms::owner_read | fs::perms::owner_exec);
      results = transfer({remote.child("read-source/stays.txt")}, local, TransferAction::Move);
      TEST_CHECK(
          results.front().outcome == Partial
          && fs::exists(backend / "read-source/stays.txt")
          && read(root / "destination/stays.txt") == "retain original"
      );
      fs::permissions(backend / "read-source", fs::perms::owner_all);
      // Cancel while the real provider is streaming, not just while a question is pending.
      {
        std::ofstream large(root / "source/large.bin");
        large.seekp(128 * 1024 * 1024 - 1);
        large.put('X');
      }
      TransferService streaming;
      TEST_CHECK(streaming.start({{native.child("large.bin")}, remote, TransferAction::Copy}));
      const auto deadline = g_get_monotonic_time() + 15000000;
      while (streaming.busy() && streaming.bytes() == 0) {
        while (g_main_context_iteration(nullptr, false)) {
        }
        TEST_CHECK(g_get_monotonic_time() < deadline);
        g_usleep(1000);
      }
      TEST_CHECK(streaming.busy());
      streaming.cancel();
      wait(streaming);
      TEST_CHECK(
          streaming.results().front().outcome == Cancelled
          && !fs::exists(backend / "large.bin")
          && fs::exists(root / "source/large.bin")
      );
      fs::remove(root / "source/large.bin");
    }
    // No temporary publication directories remain after successful/conflicted/cancelled jobs.
    for (const auto& item : fs::recursive_directory_iterator(backend)) {
      TEST_CHECK(!item.path().filename().native().starts_with(".noctalia-transfer-"));
    }
  }
} // namespace
int main(int argc, char** argv) {
  GlibPollSource glib;
  if (argc == 7 && std::string_view(argv[1]) == "--portable-provider") {
    const auto remote = Location::fromUri(argv[2]), readOnly = Location::fromUri(argv[5]);
    TEST_CHECK(transferLocationSupported(remote) && transferLocationSupported(readOnly));
    mount(remote);
    mount(readOnly);
    const files::GString uuid(g_uuid_string_random());
    const auto name = std::string("run-") + uuid.get();
    const auto backend = fs::path(argv[3]) / name, local = fs::path(argv[4]) / name;
    fs::create_directories(backend);
    fs::create_directories(local);
    portable(local, remote.child(name), backend, readOnly, argv[6]);
    mount(remote);
  } else if (argc == 5 && std::string_view(argv[1]) == "--provider") {
    const auto remote = Location::fromUri(argv[2]);
    TEST_CHECK(!remote.isNative() && transferLocationSupported(remote));
    mount(remote);
    const files::GString uuid(g_uuid_string_random());
    const auto name = std::string("run-") + uuid.get();
    const auto backend = fs::path(argv[3]) / name, local = fs::path(argv[4]) / name;
    fs::create_directories(backend);
    fs::create_directories(local);
    const auto testRoot = remote.child(name);
    run(local, testRoot, backend, true);
    write(local / "destination/to-remote.txt", "destination survives disconnect");
    TransferService disconnected;
    TEST_CHECK(disconnected.start(
        {{testRoot.child("to-remote.txt")}, Location::fromPath((local / "destination").native()), TransferAction::Move}
    ));
    wait(disconnected, [&](const auto&) {
      disconnect(testRoot);
      return TransferConflictChoice::Replace;
    });
    TEST_CHECK(
        disconnected.results().front().outcome == Failed
        && read(local / "destination/to-remote.txt") == "destination survives disconnect"
        && read(backend / "to-remote.txt") == "move upload"
    );
    mount(remote);
  } else {
    char directory[] = "/tmp/noctalia-provider-test-XXXXXX";
    TEST_CHECK(mkdtemp(directory));
    const fs::path root(directory);
    fs::create_directory(root / "remote");
    run(root, Location::fromPath((root / "remote").native()), root / "remote", false);
    for (const auto* uri : {"trash:///file", "https://example.invalid/a", "sftp://user:secret@example.invalid/a"}) {
      TEST_CHECK(!transferLocationSupported(Location::fromUri(uri)));
    }
    for (const auto* uri : {"smb://server/share", "dav://server/files", "davs://server/files"}) {
      TEST_CHECK(transferLocationSupported(Location::fromUri(uri)));
    }
    const auto smb = Location::fromUri("smb://server/share/CaseFolder");
    TEST_CHECK(transferLocationsEqual(smb, Location::fromUri("smb://SERVER/SHARE/casefolder")));
    TEST_CHECK(transferLocationInside(Location::fromUri("smb://server/share/casefolder/child"), smb));
    TEST_CHECK(!transferLocationInside(Location::fromUri("smb://server/share/casefolder-other"), smb));
    TEST_CHECK(!transferLocationsEqual(
        Location::fromUri("dav://server/files/Case"), Location::fromUri("dav://server/files/case")
    ));
    const files::GioPtr<GFileInfo> noFlags(g_file_info_new());
    TEST_CHECK(!readIsSymlink(noFlags.get()));
    g_file_info_set_file_type(noFlags.get(), G_FILE_TYPE_DIRECTORY);
    TEST_CHECK(!readIsSymlink(noFlags.get()));
    g_file_info_set_file_type(noFlags.get(), G_FILE_TYPE_SYMBOLIC_LINK);
    TEST_CHECK(readIsSymlink(noFlags.get()));
    g_file_info_set_file_type(noFlags.get(), G_FILE_TYPE_REGULAR);
    g_file_info_set_is_symlink(noFlags.get(), true);
    TEST_CHECK(readIsSymlink(noFlags.get()));
    TEST_CHECK(transferLocationsEqual(smb, Location::fromUri("smb://server:445/share/casefolder")));
    TransferResult change{Location::fromUri("dav://server/files/tree/nested/item.txt"), Partial};
    change.destination = Location::fromUri("smb://server/share/CaseFolder/item.txt");
    const std::vector<TransferResult> effects{change};
    TEST_CHECK(transferAffectsLocation(effects, Location::fromUri("dav://server/files/tree/nested")));
    TEST_CHECK(transferAffectsLocation(effects, Location::fromUri("smb://SERVER/share/casefolder")));
    TEST_CHECK(!transferAffectsLocation(effects, Location::fromUri("dav://server/files")));
    TEST_CHECK(transferAffectsLocation(effects, Location::fromUri("dav://server/files"), true));
    TEST_CHECK(!transferAffectsLocation(effects, Location::fromUri("dav://other/files"), true));
    TEST_CHECK(!transferAffectsLocation(effects, Location::fromUri("dav://server/files/tree-other"), true));
    TransferResult merged{Location::fromUri("dav://server/files/tree"), Partial};
    merged.destination = Location::fromUri("smb://server/share/CaseFolder");
    merged.children = effects;
    TEST_CHECK(transferAffectsLocation({merged}, smb));
    TransferResult folderMoved{Location::fromUri("dav://server/files/tree"), Moved};
    TEST_CHECK(transferAffectsLocation({folderMoved}, Location::fromUri("dav://server/files/tree/nested")));
    TransferService refused;
    TEST_CHECK(refused.start(
        {{Location::fromUri("sftp://user:secret@example.invalid/a")},
         Location::fromPath(root.native()),
         TransferAction::Copy}
    ));
    wait(refused);
    TEST_CHECK(refused.results().front().outcome == Failed && !refused.report().contains("secret"));
    fs::remove_all(root);
  }
  std::println("provider transfer checks passed");
}
