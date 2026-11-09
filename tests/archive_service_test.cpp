#include "event_loop/glib_poll_source.h"
#include "fs/gio_ptr.h"
#include "operations/archive_service.h"
#include "operations/archive_work.h"
#include "test_check.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <print>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;
  struct Fixture {
    fs::path root;
    Fixture() {
      std::string pattern = "/tmp/noctalia-archive-test-XXXXXX";
      const char* path = ::mkdtemp(pattern.data());
      TEST_CHECK(path);
      root = path;
      fs::create_directory(root / "tmp");
      ::setenv("TMPDIR", (root / "tmp").c_str(), 1);
    }
    ~Fixture() {
      std::error_code ignored;
      fs::remove_all(root, ignored);
    }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + 20s;
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        auto start = m_source.addPollFds(fds);
        const int timeout = m_source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), timeout < 0 ? 20 : std::min(timeout, 20)) >= 0);
        m_source.dispatch(fds, start);
      }
    }

  private:
    GlibPollSource m_source;
  };
  void write(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    TEST_CHECK(out.good());
  }
  std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  files::Location location(const fs::path& path) { return files::Location::fromPath(path.native()); }
  struct Entry {
    std::string name, data, link;
    mode_t kind = AE_IFREG;
    mode_t mode = 0644;
  };
  void tar(const fs::path& path, const std::vector<Entry>& entries) {
    const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(
        archive_write_new(), &archive_write_free
    );
    TEST_CHECK(archive_write_set_format_pax_restricted(writer.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK);
    for (const auto& item : entries) {
      const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
          archive_entry_new(), &archive_entry_free
      );
      archive_entry_set_pathname(entry.get(), item.name.c_str());
      archive_entry_set_filetype(entry.get(), item.kind);
      archive_entry_set_perm(entry.get(), item.mode);
      archive_entry_set_size(
          entry.get(), item.kind == AE_IFREG && item.link.empty() ? static_cast<la_int64_t>(item.data.size()) : 0
      );
      if (!item.link.empty()) {
        if (item.kind == AE_IFLNK) {
          archive_entry_set_symlink(entry.get(), item.link.c_str());
        } else {
          archive_entry_set_hardlink(entry.get(), item.link.c_str());
        }
      }
      TEST_CHECK(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
      if (item.kind == AE_IFREG && item.link.empty() && !item.data.empty()) {
        TEST_CHECK(
            archive_write_data(writer.get(), item.data.data(), item.data.size())
            == static_cast<la_ssize_t>(item.data.size())
        );
      }
    }
    TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
  }
  void wait(files::ArchiveService& service, Pump& pump) {
    pump.until([&] { return !service.busy(); });
  }
  void testRoundTrips(const Fixture& fixture, Pump& pump) {
    const auto src = fixture.root / "source";
    fs::create_directories(src / "empty");
    write(src / "hello.txt", "archive content");
    write(src / "café\nnotes.txt", "Unicode and newline");
    fs::create_symlink("hello.txt", src / "link");
    TEST_CHECK(!files::archiveFormats().empty());
    for (const auto& format : files::archiveFormats()) {
      const auto archive = fixture.root / ("roundtrip" + std::string(format.suffix));
      files::ArchiveService service;
      TEST_CHECK(service.compress({location(src)}, location(archive), format.format));
      wait(service, pump);
      if (service.error()) {
        throw std::runtime_error(service.error()->message);
      }
      TEST_CHECK(!service.error() && fs::exists(archive));
      TEST_CHECK(service.inspect(location(archive)));
      wait(service, pump);
      TEST_CHECK(!service.error() && service.catalog() && service.catalog()->entries.size() >= 5);
      const auto dest = fixture.root / ("unpacked" + std::to_string(static_cast<int>(format.format)));
      TEST_CHECK(service.extract(location(archive), location(dest)));
      wait(service, pump);
      TEST_CHECK(
          !service.error()
          && read(dest / "source/hello.txt") == "archive content"
          && read(dest / "source/café\nnotes.txt") == "Unicode and newline"
          && fs::is_directory(dest / "source/empty")
      );
      TEST_CHECK(fs::is_symlink(dest / "source/link") && fs::read_symlink(dest / "source/link") == "hello.txt");
      TEST_CHECK(service.openMember("source/hello.txt"));
      wait(service, pump);
      const auto opened = service.takeOpenedFile();
      TEST_CHECK(opened && read(*opened->nativePath()) == "archive content");
      const auto temporary = *opened->nativePath();
      service.shutdown();
      wait(service, pump);
      TEST_CHECK(!fs::exists(temporary));
    }
  }
  void testConflicts(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "conflict.txt", archive = fixture.root / "conflict.zip";
    write(source, "first");
    files::ArchiveService service;
    TEST_CHECK(service.compress({location(source)}, location(archive), files::ArchiveFormat::Zip));
    wait(service, pump);
    const auto original = read(archive);
    write(source, "second content");
    for (const auto choice :
         {files::TransferConflictChoice::Skip, files::TransferConflictChoice::KeepBoth,
          files::TransferConflictChoice::Replace}) {
      TEST_CHECK(service.compress({location(source)}, location(archive), files::ArchiveFormat::Zip));
      pump.until([&] { return service.conflict().has_value(); });
      const auto conflict = service.conflict();
      TEST_CHECK(service.resolve(conflict->id, choice));
      wait(service, pump);
      if (choice != files::TransferConflictChoice::Replace) {
        TEST_CHECK(read(archive) == original);
      } else {
        TEST_CHECK(!service.error() && read(archive) != original);
      }
    }
    TEST_CHECK(fs::exists(fixture.root / "conflict (1).zip"));
  }
  void testHostile(const Fixture& fixture, Pump& pump) {
    const std::vector<std::vector<Entry>> cases{
        {{"../escaped", "bad", {}}},
        {{"/absolute", "bad", {}}},
        {{"C:/absolute", "bad", {}}},
        {{"..\\escaped", "bad", {}}},
        {{"link", {}, "../outside", AE_IFLNK}},
        {{"link", {}, "/outside", AE_IFLNK}},
        {{"redirect", {}, ".", AE_IFLNK}, {"escape", {}, "redirect/../outside", AE_IFLNK}},
        {{"dir", {}, "target", AE_IFLNK}, {"dir/file", "bad", {}}},
        {{"dir/file", "bad", {}}, {"dir", {}, "target", AE_IFLNK}},
        {{"same", "one", {}}, {"same", "two", {}}},
        {{"pipe", {}, {}, AE_IFIFO}},
        {{"hard", {}, "../outside", AE_IFREG}}
    };
    std::size_t index = 0;
    for (const auto& entries : cases) {
      const auto source = fixture.root / ("hostile" + std::to_string(index) + ".tar"),
                 dest = fixture.root / ("rejected" + std::to_string(index++));
      tar(source, entries);
      files::ArchiveService service;
      TEST_CHECK(service.extract(location(source), location(dest)));
      wait(service, pump);
      TEST_CHECK(service.error() && !fs::exists(dest));
    }
    TEST_CHECK(!fs::exists(fixture.root / "escaped"));
  }
  void testLimitsAndTruncation(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "limits.tar";
    tar(source, {{"a/b/file", std::string(4096, 'x'), {}}});
    std::vector<files::ArchiveLimits> limits(5);
    limits[0].maxExpandedBytes = 100;
    limits[1].maxEntries = 1;
    limits[2].maxDepth = 1;
    limits[3].maxMetadataBytes = 3;
    limits[4].maxArchiveBytes = 8;
    for (std::size_t i = 0; i < limits.size(); ++i) {
      files::ArchiveService service(limits[i]);
      const auto dest = fixture.root / ("limited" + std::to_string(i));
      TEST_CHECK(service.extract(location(source), location(dest)));
      wait(service, pump);
      TEST_CHECK(service.error() && !fs::exists(dest));
    }
    const auto truncated = fixture.root / "truncated.tar";
    fs::copy_file(source, truncated);
    fs::resize_file(truncated, 515);
    files::ArchiveService service;
    const auto dest = fixture.root / "truncated-output";
    TEST_CHECK(service.extract(location(truncated), location(dest)));
    wait(service, pump);
    TEST_CHECK(service.error() && !fs::exists(dest));
  }
  void testLinksAndSelection(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "links.tar";
    tar(source,
        {{"dir/real", "linked data", {}, AE_IFREG, 04777},
         {"alias", {}, "dir/real", AE_IFREG},
         {"safe", {}, "dir/../dir/real", AE_IFLNK}});
    files::ArchiveService service;
    TEST_CHECK(service.inspect(location(source)));
    wait(service, pump);
    TEST_CHECK(!service.error());
    const auto dest = fixture.root / "selected";
    TEST_CHECK(service.extract(location(source), location(dest), {"alias"}));
    wait(service, pump);
    TEST_CHECK(!service.error() && read(dest / "alias") == "linked data" && !fs::exists(dest / "dir/real"));
    struct stat info{};
    TEST_CHECK(
        ::stat((dest / "alias").c_str(), &info) == 0 && (info.st_mode & S_ISUID) == 0 && info.st_uid == ::getuid()
    );
    write(source, read(source) + "changed");
    TEST_CHECK(service.extract(location(source), location(fixture.root / "changed"), {"alias"}));
    wait(service, pump);
    TEST_CHECK(service.error() && !fs::exists(fixture.root / "changed"));
    TEST_CHECK(service.extract(location(source), location(fixture.root / "changed-all")));
    wait(service, pump);
    TEST_CHECK(service.error() && !fs::exists(fixture.root / "changed-all"));
  }
  void testHardlinkOutputLimit(const Fixture& fixture, Pump& pump) {
    const auto path = fixture.root / "link-limit.tar";
    tar(path, {{"body", "12345", {}}, {"alias", {}, "body"}});
    files::ArchiveLimits limits;
    limits.maxExpandedBytes = 9;
    files::ArchiveService service(limits);
    TEST_CHECK(service.extract(location(path), location(fixture.root / "link-limit")));
    wait(service, pump);
    TEST_CHECK(service.error() && !fs::exists(fixture.root / "link-limit"));
  }
  void
  encryptedZip(const fs::path& path, std::string_view encryption = "aes256", const std::string& name = "secret.txt") {
    const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(
        archive_write_new(), &archive_write_free
    );
    TEST_CHECK(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK);
    TEST_CHECK(
        archive_write_set_options(writer.get(), ("zip:encryption=" + std::string(encryption)).c_str()) == ARCHIVE_OK
    );
    TEST_CHECK(archive_write_set_passphrase(writer.get(), "test password") == ARCHIVE_OK);
    TEST_CHECK(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK);
    const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
        archive_entry_new(), &archive_entry_free
    );
    archive_entry_set_pathname(entry.get(), name.c_str());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0600);
    archive_entry_set_size(entry.get(), 6);
    TEST_CHECK(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_data(writer.get(), "secret", 6) == 6);
    TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
  }
  files::ArchivePasswordRequest passwordRequest(files::ArchiveService& service, Pump& pump) {
    pump.until([&] { return service.passwordRequest().has_value() || !service.busy(); });
    TEST_CHECK(service.busy() && service.phase() == files::ArchivePhase::WaitingPassword && !service.error());
    TEST_CHECK(service.passwordRequest());
    return *service.passwordRequest();
  }
  void testEncryptedRead(const Fixture& fixture, Pump& pump) {
    for (const std::string encryption : {"aes256", "zipcrypt"}) {
      const auto path = fixture.root / (encryption + ".zip");
      encryptedZip(path, encryption);
      files::ArchiveService service;
      TEST_CHECK(service.inspect(location(path)));
      wait(service, pump);
      TEST_CHECK(!service.error() && service.catalog() && service.catalog()->encrypted);
      TEST_CHECK(service.catalog()->entries.size() == 1 && !service.passwordRequest());
      TEST_CHECK(service.openMember("secret.txt"));
      const auto first = passwordRequest(service, pump);
      TEST_CHECK(first.source == location(path) && !first.retry);
      TEST_CHECK(!service.providePassword(first.id + 1, "test password"));
      TEST_CHECK(!service.providePassword(first.id, ""));
      TEST_CHECK(!service.providePassword(first.id, std::string(1025, 'x')));
      TEST_CHECK(!service.providePassword(first.id, std::string("x\0y", 3)));
      TEST_CHECK(!service.inspect(location(path)));
      TEST_CHECK(service.providePassword(first.id, "wrong password"));
      const auto retry = passwordRequest(service, pump);
      TEST_CHECK(retry.id != first.id && retry.retry);
      TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
      TEST_CHECK(!service.providePassword(first.id, "test password"));
      TEST_CHECK(service.providePassword(retry.id, "test password"));
      wait(service, pump);
      TEST_CHECK(!service.error() && !service.passwordRequest());
      const auto opened = service.takeOpenedFile();
      TEST_CHECK(opened && read(*opened->nativePath()) == "secret");
      struct stat mode{};
      TEST_CHECK(::stat(opened->nativePath()->c_str(), &mode) == 0 && (mode.st_mode & 0111) == 0);
      service.shutdown();
      wait(service, pump);
      TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
    }
  }
  void testEncryptedCreation(const Fixture& fixture, Pump& pump) {
    TEST_CHECK(files::archiveEncryptionAvailable());
    const auto source = fixture.root / "encrypted-source.txt";
    const auto output = fixture.root / "created-encrypted.zip";
    write(source, "encrypted payload");
    files::ArchiveService service;
    TEST_CHECK(!service.compress({location(source)}, location(output), files::ArchiveFormat::Tar, "test password"));
    TEST_CHECK(
        !service.compress({location(source)}, location(output), files::ArchiveFormat::Zip, std::string(1025, 'x'))
    );
    TEST_CHECK(!fs::exists(output));
    TEST_CHECK(service.compress({location(source)}, location(output), files::ArchiveFormat::Zip, "test password"));
    wait(service, pump);
    TEST_CHECK(!service.error() && read(source) == "encrypted payload");
    // Read independently through the system provider; the writer must not emit plaintext ZIP data.
    const std::unique_ptr<struct archive, decltype(&archive_read_free)> reader(archive_read_new(), &archive_read_free);
    TEST_CHECK(archive_read_support_format_zip(reader.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_read_open_filename(reader.get(), output.c_str(), 65536) == ARCHIVE_OK);
    struct archive_entry* entry = nullptr;
    TEST_CHECK(archive_read_next_header(reader.get(), &entry) == ARCHIVE_OK && archive_entry_is_encrypted(entry) > 0);
    char bytes[32]{};
    TEST_CHECK(archive_read_data(reader.get(), bytes, sizeof(bytes)) < 0);
    const auto destination = fixture.root / "decrypted";
    TEST_CHECK(service.extract(location(output), location(destination)));
    const auto request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, "test password"));
    wait(service, pump);
    TEST_CHECK(!service.error() && read(destination / source.filename()) == "encrypted payload");
    const auto file = fixture.root / "not-a-folder";
    write(file, "untouched");
    TEST_CHECK(service.compress({location(file)}, location(file / "failure.tar"), files::ArchiveFormat::Tar));
    wait(service, pump);
    TEST_CHECK(service.error() && read(file) == "untouched");
  }
  void testPasswordCancellationAndChanges(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "cancel-encrypted.zip";
    encryptedZip(source);
    const auto output = fixture.root / "cancel-encrypted-output";
    files::ArchiveService service;
    TEST_CHECK(service.extract(location(source), location(output)));
    const auto request = passwordRequest(service, pump);
    TEST_CHECK(fs::is_empty(fixture.root / "tmp") && !fs::exists(output));
    service.cancel();
    wait(service, pump);
    TEST_CHECK(
        service.error() && service.error()->kind == files::FileErrorKind::Cancelled && !service.passwordRequest()
    );
    TEST_CHECK(!service.providePassword(request.id, "test password"));
    TEST_CHECK(service.extract(location(source), location(output)));
    const auto next = passwordRequest(service, pump);
    TEST_CHECK(next.id != request.id && !next.retry);
    const auto replacement = fixture.root / "replacement-encrypted.zip";
    encryptedZip(replacement);
    fs::rename(replacement, source);
    TEST_CHECK(service.providePassword(next.id, "test password"));
    wait(service, pump);
    TEST_CHECK(service.error() && service.error()->kind == files::FileErrorKind::Changed);
    TEST_CHECK(!fs::exists(output) && fs::is_empty(fixture.root / "tmp"));
    TEST_CHECK(service.extract(location(source), location(output)));
    const auto cancelAfterAnswer = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(cancelAfterAnswer.id, "test password"));
    service.cancel();
    wait(service, pump);
    TEST_CHECK(service.error() && !fs::exists(output) && fs::is_empty(fixture.root / "tmp"));
    {
      auto owner = std::make_unique<files::ArchiveService>();
      TEST_CHECK(owner->extract(location(source), location(output)));
      (void)passwordRequest(*owner, pump);
      owner.reset();
    }
    TEST_CHECK(!fs::exists(output) && fs::is_empty(fixture.root / "tmp"));
  }
  void testEncryptedBoundaries(const Fixture& fixture, Pump& pump) {
    const auto hostile = fixture.root / "encrypted-hostile.zip";
    encryptedZip(hostile, "aes256", "../escape");
    files::ArchiveService service;
    TEST_CHECK(service.extract(location(hostile), location(fixture.root / "hostile-output")));
    wait(service, pump);
    TEST_CHECK(service.error() && !service.passwordRequest() && !fs::exists(fixture.root / "escape"));
    const auto source = fixture.root / "encrypted-limited.zip";
    encryptedZip(source);
    files::ArchiveLimits limits;
    limits.maxExpandedBytes = 1;
    files::ArchiveService limited(limits);
    TEST_CHECK(limited.extract(location(source), location(fixture.root / "limited-output")));
    wait(limited, pump);
    TEST_CHECK(limited.error() && !limited.passwordRequest() && !fs::exists(fixture.root / "limited-output"));
    const auto output = fixture.root / "encrypted-conflict";
    fs::create_directory(output);
    write(output / "existing", "preserved");
    TEST_CHECK(service.extract(location(source), location(output)));
    const auto request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, "test password"));
    pump.until([&] { return service.conflict().has_value() || !service.busy(); });
    TEST_CHECK(service.conflict());
    TEST_CHECK(service.resolve(service.conflict()->id, files::TransferConflictChoice::Cancel));
    wait(service, pump);
    TEST_CHECK(read(output / "existing") == "preserved" && !fs::exists(output / "secret.txt"));
    TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
  }
  void testEncryptedCorruption(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "corrupt-encrypted.zip";
    encryptedZip(source);
    auto bytes = read(source);
    const auto u16 = [&](std::size_t offset) {
      return static_cast<unsigned char>(bytes.at(offset)) | (static_cast<unsigned char>(bytes.at(offset + 1)) << 8);
    };
    const std::size_t data = 30 + u16(26) + u16(28);
    // AES-256's 16-byte salt and 2-byte password verifier precede ciphertext.
    bytes.at(data + 18) = static_cast<char>(static_cast<unsigned char>(bytes.at(data + 18)) ^ 1U);
    write(source, bytes);
    const auto output = fixture.root / "corrupt-encrypted-output";
    files::ArchiveService service;
    TEST_CHECK(service.extract(location(source), location(output)));
    const auto request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, "test password"));
    wait(service, pump);
    TEST_CHECK(service.error() && !service.passwordRequest());
    TEST_CHECK(!fs::exists(output) && fs::is_empty(fixture.root / "tmp"));
  }
  int boundedWriteFailure() {
    Fixture fixture;
    const auto source = fixture.root / "large.txt";
    std::string data(8192, '\0');
    std::uint32_t seed = 17;
    for (auto& byte : data) {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      byte = static_cast<char>(seed & 255U);
    }
    write(source, data);
    std::signal(SIGXFSZ, SIG_IGN);
    const rlimit limit{1024, 1024};
    TEST_CHECK(::setrlimit(RLIMIT_FSIZE, &limit) == 0);
    const files::CancelToken cancel;
    for (const auto format : {files::ArchiveFormat::Tar, files::ArchiveFormat::Zip}) {
      files::archive_work::Progress progress;
      files::archive_work::Passwords passwords;
      if (format == files::ArchiveFormat::Zip) {
        passwords.values.emplace_back("test password");
      }
      bool failed = false;
      try {
        auto prepared = files::archive_work::compress(
            {location(source)}, location(fixture.root / "failed-output"), format, {}, cancel, progress, passwords
        );
        (void)files::archive_work::cleanup({prepared.stage});
      } catch (const std::exception&) {
        failed = true;
      }
      TEST_CHECK(failed && !fs::exists(fixture.root / "failed-output") && fs::is_empty(fixture.root / "tmp"));
    }
    return 0;
  }
  void testCancelAndDestruction(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "cancel.txt", output = fixture.root / "cancel.zip";
    write(source, std::string(1024 * 1024, 'x'));
    files::ArchiveService service;
    TEST_CHECK(service.compress({location(source)}, location(output), files::ArchiveFormat::Zip));
    service.cancel();
    wait(service, pump);
    TEST_CHECK(!fs::exists(output) && service.error());
    write(output, "keep me");
    auto owner = std::make_unique<files::ArchiveService>();
    TEST_CHECK(owner->compress({location(source)}, location(output), files::ArchiveFormat::Zip));
    pump.until([&] { return owner->conflict().has_value(); });
    owner.reset();
    pump.until([&] { return fs::is_empty(fixture.root / "tmp"); });
    TEST_CHECK(read(output) == "keep me");
  }
} // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--bounded-write-failure") {
      return boundedWriteFailure();
    }
    Fixture fixture;
    Pump pump;
    testRoundTrips(fixture, pump);
    testConflicts(fixture, pump);
    testHostile(fixture, pump);
    testLimitsAndTruncation(fixture, pump);
    testLinksAndSelection(fixture, pump);
    testHardlinkOutputLimit(fixture, pump);
    testEncryptedRead(fixture, pump);
    testEncryptedCreation(fixture, pump);
    testPasswordCancellationAndChanges(fixture, pump);
    testEncryptedBoundaries(fixture, pump);
    testEncryptedCorruption(fixture, pump);
    testCancelAndDestruction(fixture, pump);
    files::GioPtr<GSubprocess> child(
        g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, nullptr, "/proc/self/exe", "--bounded-write-failure", nullptr)
    );
    TEST_CHECK(child && g_subprocess_wait_check(child.get(), nullptr, nullptr));
    TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
    std::println("archive checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "archive test: {}", error.what());
    return 1;
  }
}
