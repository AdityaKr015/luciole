#include "event_loop/glib_poll_source.h"
#include "fs/gio_ptr.h"
#include "operations/archive_input.h"
#include "operations/archive_service.h"
#include "operations/archive_work.h"
#include "operations/archive_xz_input.h"
#include "test_check.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <format>
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
  void tar(const fs::path& path, const std::vector<Entry>& entries, int filter = ARCHIVE_FILTER_NONE) {
    const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(
        archive_write_new(), &archive_write_free
    );
    TEST_CHECK(archive_write_set_format_pax_restricted(writer.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_add_filter(writer.get(), filter) == ARCHIVE_OK);
    TEST_CHECK(archive_write_set_bytes_per_block(writer.get(), 0) == ARCHIVE_OK);
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
  std::vector<fs::path>
  splitZip(const fs::path& folder, const std::string& name, const std::string& bytes, std::size_t chunk) {
    fs::create_directories(folder);
    std::vector<fs::path> result;
    for (std::size_t offset = 0; offset < bytes.size(); offset += chunk) {
      const auto path = folder / (name + std::format(".{:03}", result.size() + 1));
      result.push_back(path);
    }
    // Discovery must use volume numbers, independently of directory insertion order.
    for (std::size_t i = result.size(); i > 0; --i) {
      write(result[i - 1], bytes.substr((i - 1) * chunk, chunk));
    }
    return result;
  }
  template <typename Action> void rejects(Action action) {
    bool failed = false;
    try {
      action();
    } catch (const files::FileErrorException&) {
      failed = true;
    }
    TEST_CHECK(failed);
  }
  void testSplitInput(const Fixture& fixture) {
    std::string bytes;
    for (std::size_t i = 0; i < 131072; ++i) {
      bytes += static_cast<char>(i % 251);
    }
    const auto parts = splitZip(fixture.root / "seek-parts", "virtual.zip", bytes, 1024);
    files::archive_work::Progress progress;
    files::CancelToken cancel;
    auto work = files::archive_work::workFor({}, cancel, progress);
    files::archive_work::Input input(location(parts.front()), work);
    TEST_CHECK(input.split() && !input.fingerprint().empty());
    std::array<char, 65536> buffer{};
    TEST_CHECK(input.seek(4093, SEEK_SET) == 4093);
    TEST_CHECK(input.read(buffer) == buffer.size());
    TEST_CHECK(std::string_view(buffer.data(), buffer.size()) == std::string_view(bytes).substr(4093, buffer.size()));
    TEST_CHECK(input.seek(-4, SEEK_CUR) == 4093 + 65536 - 4);
    TEST_CHECK(input.seek(-7, SEEK_END) == static_cast<std::int64_t>(bytes.size() - 7));
    TEST_CHECK(
        input.read(buffer) == 7
        && std::string_view(buffer.data(), 7) == std::string_view(bytes).substr(bytes.size() - 7)
    );
    TEST_CHECK(input.read(buffer) == 0);
    rejects([&] { (void)input.seek(INT64_MIN, SEEK_END); });
    TEST_CHECK(input.seek(INT64_MAX, SEEK_SET) == INT64_MAX);
    rejects([&] { (void)input.seek(1, SEEK_CUR); });
    rejects([&] { (void)input.seek(0, -99); });
    TEST_CHECK(input.seek(0, SEEK_SET) == 0);
    input.unchanged();
    write(parts[1], "replacement");
    rejects([&] { input.unchanged(); });
    cancel.cancel();
    rejects([&] { (void)input.read(buffer); });
  }
  void testSplitZip(const Fixture& fixture, Pump& pump) {
    const auto plain = fixture.root / "split-source.txt";
    write(plain, "payload spanning split ZIP parts");
    const auto archive = fixture.root / "before-splitting.zip";
    files::ArchiveService service;
    TEST_CHECK(service.compress({location(plain)}, location(archive), files::ArchiveFormat::Zip));
    wait(service, pump);
    TEST_CHECK(!service.error());
    if (::geteuid() != 0) {
      const auto restricted = fixture.root / "unlistable-parent";
      fs::create_directory(restricted);
      fs::copy_file(archive, restricted / "known.zip");
      TEST_CHECK(::chmod(restricted.c_str(), 0111) == 0);
      TEST_CHECK(service.inspect(location(restricted / "known.zip")));
      wait(service, pump);
      TEST_CHECK(!service.error());
      TEST_CHECK(::chmod(restricted.c_str(), 0700) == 0);
    }
    const auto parts = splitZip(fixture.root / "zip-parts", "café.ZIP", read(archive), 17);
    TEST_CHECK(parts.size() > 2 && files::isSupportedArchive(location(parts[0]), {}));
    TEST_CHECK(!files::isSupportedArchive(location(parts[1]), "application/zip"));
    TEST_CHECK(files::archiveFolderName(location(parts[0])) == "café");
    TEST_CHECK(service.inspect(location(parts.front())));
    wait(service, pump);
    TEST_CHECK(!service.error() && service.catalog() && service.catalog()->source == location(parts.front()));
    const auto known = service.catalog();
    const auto output = fixture.root / "split-output";
    TEST_CHECK(service.extract(location(parts.front()), location(output), {plain.filename().native()}));
    wait(service, pump);
    TEST_CHECK(!service.error() && read(output / plain.filename()) == read(plain));
    TEST_CHECK(service.openMember(plain.filename().native()));
    wait(service, pump);
    const auto opened = service.takeOpenedFile();
    TEST_CHECK(opened && read(*opened->nativePath()) == read(plain));
    service.shutdown();
    wait(service, pump);
    TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
    write(parts[1], "changed second volume");
    files::ArchiveService changed;
    TEST_CHECK(changed.extract(location(parts.front()), location(fixture.root / "stale-split"), {}, known));
    wait(changed, pump);
    TEST_CHECK(changed.error() && !fs::exists(fixture.root / "stale-split"));
  }
  void testSplitFailures(const Fixture& fixture, Pump& pump) {
    const auto archive = fixture.root / "full-encrypted.zip";
    encryptedZip(archive);
    const auto bytes = read(archive);
    for (unsigned failure = 0; failure < 9; ++failure) {
      const auto folder = fixture.root / ("invalid-parts-" + std::to_string(failure));
      const auto parts = splitZip(folder, "test.zip", bytes, 17);
      if (failure == 0) {
        fs::remove(parts[1]);
      } else if (failure == 1) {
        fs::remove(parts.back());
      } else if (failure == 2) {
        fs::resize_file(parts[1], 0);
      } else if (failure == 3) {
        fs::remove(parts[1]);
        fs::create_symlink(parts[0], parts[1]);
      } else if (failure == 4) {
        fs::remove(parts[1]);
        fs::create_hard_link(parts[0], parts[1]);
      } else if (failure == 5) {
        fs::remove(parts[1]);
        TEST_CHECK(::mkfifo(parts[1].c_str(), 0600) == 0);
      } else if (failure == 6) {
        write(folder / "test.zip.129", "unsupported extra volume");
      } else if (failure == 7) {
        auto damaged = read(parts.back());
        damaged.pop_back();
        write(parts.back(), damaged);
      }
      files::ArchiveLimits limits;
      if (failure == 8) {
        limits.maxArchiveBytes = bytes.size() - 1;
      }
      files::ArchiveService service(limits);
      const auto output = folder / "output";
      TEST_CHECK(service.extract(location(parts.front()), location(output)));
      wait(service, pump);
      TEST_CHECK(service.error() && !service.passwordRequest() && !fs::exists(output));
      TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
    }
    // A middle part must never be treated as an independently recoverable archive.
    const auto parts = splitZip(fixture.root / "middle-part", "test.zip", bytes, 17);
    files::ArchiveService middle;
    TEST_CHECK(middle.inspect(location(parts[1])));
    wait(middle, pump);
    TEST_CHECK(middle.error() && !middle.catalog());
    // Numbered parts do not opt TAR into the split ZIP reader.
    const auto tarFile = fixture.root / "not-a-zip.tar";
    tar(tarFile, {{"file", "payload", {}}});
    const auto fake = splitZip(fixture.root / "fake-zip", "fake.zip", read(tarFile), 1024);
    files::ArchiveService fakeZip;
    TEST_CHECK(fakeZip.inspect(location(fake.front())));
    wait(fakeZip, pump);
    TEST_CHECK(fakeZip.error() && !fakeZip.catalog());
  }
  void testSplitPasswordAndChanges(const Fixture& fixture, Pump& pump) {
    const auto archive = fixture.root / "split-encrypted.zip";
    encryptedZip(archive);
    const auto bytes = read(archive);
    const auto parts = splitZip(fixture.root / "encrypted-parts", "secret.zip", bytes, 17);
    const auto output = fixture.root / "unlocked-parts";
    files::ArchiveService service;
    TEST_CHECK(service.extract(location(parts.front()), location(output)));
    const auto request = passwordRequest(service, pump);
    TEST_CHECK(service.providePassword(request.id, "wrong"));
    const auto retry = passwordRequest(service, pump);
    TEST_CHECK(retry.retry && service.providePassword(retry.id, "test password"));
    wait(service, pump);
    TEST_CHECK(!service.error() && read(output / "secret.txt") == "secret");
    TEST_CHECK(service.extract(location(parts.front()), location(fixture.root / "split-cancelled")));
    (void)passwordRequest(service, pump);
    service.cancel();
    wait(service, pump);
    TEST_CHECK(service.error() && !fs::exists(fixture.root / "split-cancelled"));
    TEST_CHECK(service.extract(location(parts.front()), location(fixture.root / "split-replaced")));
    const auto replacement = passwordRequest(service, pump);
    const auto original = read(parts[1]);
    // Same bytes and pathname, a different inode: all volumes participate in the retry stamp.
    fs::remove(parts[1]);
    write(parts[1], original);
    TEST_CHECK(service.providePassword(replacement.id, "test password"));
    wait(service, pump);
    TEST_CHECK(service.error() && service.error()->kind == files::FileErrorKind::Changed);
    TEST_CHECK(!fs::exists(fixture.root / "split-replaced") && fs::is_empty(fixture.root / "tmp"));
    const auto newSet = splitZip(fixture.root / "extended-parts", "secret.zip", bytes, 17);
    files::ArchiveService extended;
    TEST_CHECK(extended.extract(location(newSet.front()), location(fixture.root / "split-extended")));
    const auto pending = passwordRequest(extended, pump);
    write(newSet.back().parent_path() / std::format("secret.zip.{:03}", newSet.size() + 1), "extra data");
    TEST_CHECK(extended.providePassword(pending.id, "test password"));
    wait(extended, pump);
    TEST_CHECK(extended.error() && !fs::exists(fixture.root / "split-extended"));
  }
  struct CompressedTarCase {
    files::ArchiveFormat format;
    files::ArchiveCompression compression;
    int filter;
    std::string_view suffix, shortSuffix, mime;
  };
  constexpr std::array compressedTars{
      CompressedTarCase{
          files::ArchiveFormat::TarBzip2, files::ArchiveCompression::Bzip2, ARCHIVE_FILTER_BZIP2, ".tar.bz2", ".tbz2",
          "application/x-bzip2-compressed-tar"
      },
      CompressedTarCase{
          files::ArchiveFormat::TarZstd, files::ArchiveCompression::Zstd, ARCHIVE_FILTER_ZSTD, ".tar.zst", ".tzst",
          "application/x-zstd-compressed-tar"
      }
  };
  void testCompressedTarProviders(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "compressed-payload.txt";
    write(source, "compressed TAR payload");
    for (const auto& item : compressedTars) {
      const bool available = files::archiveCompressionAvailable(item.compression);
      TEST_CHECK(
          available
          == (item.filter == ARCHIVE_FILTER_BZIP2 ? archive_bzlib_version() != nullptr
                                                  : archive_libzstd_version() != nullptr)
      );
      const auto formats = files::archiveFormats();
      const auto format = std::ranges::find(formats, item.format, &files::ArchiveFormatInfo::format);
      TEST_CHECK(available == (format != formats.end()));
      const auto path = fixture.root / ("native" + std::string(item.suffix));
      TEST_CHECK(files::isSupportedArchive(location(path), {}) == available);
      if (!available) {
        continue;
      }
      TEST_CHECK(format->compression == item.compression && format->suffix == item.suffix);
      files::ArchiveService service;
      TEST_CHECK(!service.compress({location(source)}, location(path), item.format, "not supported"));
      TEST_CHECK(service.compress({location(source)}, location(path), item.format));
      wait(service, pump);
      TEST_CHECK(!service.error());
      const auto original = read(path);
      const std::unique_ptr<struct archive, decltype(&archive_read_free)> reader(
          archive_read_new(), &archive_read_free
      );
      TEST_CHECK(archive_read_support_filter_by_code(reader.get(), item.filter) == ARCHIVE_OK);
      TEST_CHECK(archive_read_support_format_tar(reader.get()) == ARCHIVE_OK);
      TEST_CHECK(archive_read_open_filename(reader.get(), path.c_str(), 65536) == ARCHIVE_OK);
      struct archive_entry* header = nullptr;
      TEST_CHECK(archive_read_next_header(reader.get(), &header) == ARCHIVE_OK);
      TEST_CHECK(archive_filter_code(reader.get(), 0) == item.filter);
      std::array<char, 64> bytes{};
      const auto count = archive_read_data(reader.get(), bytes.data(), bytes.size());
      TEST_CHECK(count == 22 && std::string_view(bytes.data(), static_cast<std::size_t>(count)) == read(source));
      const auto renamed = fixture.root / ("aliased" + std::string(item.shortSuffix));
      fs::copy_file(path, renamed);
      TEST_CHECK(files::archiveFolderName(location(renamed)) == "aliased");
      TEST_CHECK(service.inspect(location(renamed)));
      wait(service, pump);
      TEST_CHECK(!service.error() && service.catalog());
      const auto known = service.catalog();
      write(renamed, original);
      const auto destination = fixture.root / ("stale" + std::string(item.shortSuffix));
      TEST_CHECK(service.extract(location(renamed), location(destination), {}, known));
      wait(service, pump);
      TEST_CHECK(service.error() && service.error()->kind == files::FileErrorKind::Changed && !fs::exists(destination));
      TEST_CHECK(service.compress({location(source)}, location(path), item.format));
      pump.until([&] { return service.conflict().has_value() || !service.busy(); });
      TEST_CHECK(service.conflict() && service.resolve(service.conflict()->id, files::TransferConflictChoice::Skip));
      wait(service, pump);
      TEST_CHECK(read(path) == original && fs::is_empty(fixture.root / "tmp"));
      const auto cancelled = fixture.root / ("cancelled" + std::string(item.suffix));
      TEST_CHECK(service.compress({location(source)}, location(cancelled), item.format));
      service.cancel();
      wait(service, pump);
      TEST_CHECK(service.error() && !fs::exists(cancelled) && fs::is_empty(fixture.root / "tmp"));
    }
  }
  void testCompressedTarFailures(const Fixture& fixture, Pump& pump) {
    for (const auto& item : compressedTars) {
      if (!files::archiveCompressionAvailable(item.compression)) {
        continue;
      }
      for (unsigned failure = 0; failure < 5; ++failure) {
        const auto name = std::to_string(item.filter) + "-" + std::to_string(failure);
        const auto source = fixture.root / (name + std::string(item.suffix));
        const auto destination = fixture.root / ("rejected-compressed-" + name);
        const auto entry = failure == 0 ? Entry{"../escaped", "bad", {}}
            : failure == 1              ? Entry{"link", {}, "../escaped", AE_IFLNK}
                                        : Entry{"payload", std::string(129536, 'x'), {}};
        tar(source, {entry}, item.filter);
        files::ArchiveLimits limits;
        if (failure == 2) {
          limits.maxExpandedBytes = 1024;
        } else if (failure == 3) {
          fs::resize_file(source, fs::file_size(source) / 2);
        } else if (failure == 4) {
          // Incomplete framing after TAR end markers must not publish a prefix.
          fs::resize_file(source, fs::file_size(source) - 1);
        }
        files::ArchiveService service(limits);
        TEST_CHECK(service.extract(location(source), location(destination)));
        wait(service, pump);
        if (!service.error()) {
          std::println(stderr, "Compressed TAR failure fixture accepted: filter={}, failure={}", item.filter, failure);
        }
        TEST_CHECK(service.error() && !fs::exists(destination) && fs::is_empty(fixture.root / "tmp"));
      }
    }
    TEST_CHECK(!fs::exists(fixture.root / "escaped"));
  }
  void testCompressedPaddingLimit(const Fixture& fixture, Pump& pump) {
    const auto plain = fixture.root / "padded-container.tar";
    tar(plain, {{"file", "abc", {}}});
    const auto data = read(plain) + std::string(4U * 1024U * 1024U, '\0');
    for (const auto& item : compressedTars) {
      if (!files::archiveCompressionAvailable(item.compression)) {
        continue;
      }
      const auto source = fixture.root / ("padded" + std::string(item.suffix));
      const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(
          archive_write_new(), &archive_write_free
      );
      TEST_CHECK(archive_write_set_format_raw(writer.get()) == ARCHIVE_OK);
      TEST_CHECK(archive_write_add_filter(writer.get(), item.filter) == ARCHIVE_OK);
      TEST_CHECK(archive_write_set_bytes_per_block(writer.get(), 0) == ARCHIVE_OK);
      TEST_CHECK(archive_write_open_filename(writer.get(), source.c_str()) == ARCHIVE_OK);
      const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
          archive_entry_new(), &archive_entry_free
      );
      archive_entry_set_pathname(entry.get(), "data");
      archive_entry_set_filetype(entry.get(), AE_IFREG);
      archive_entry_set_size(entry.get(), static_cast<la_int64_t>(data.size()));
      TEST_CHECK(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
      TEST_CHECK(archive_write_data(writer.get(), data.data(), data.size()) == static_cast<la_ssize_t>(data.size()));
      TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
      files::ArchiveLimits limits;
      limits.maxExpandedBytes = 128;
      limits.maxMetadataBytes = 256;
      limits.maxEntries = 4;
      files::ArchiveService service(limits);
      const auto output = fixture.root / ("padding-rejected" + std::to_string(item.filter));
      TEST_CHECK(service.extract(location(source), location(output)));
      wait(service, pump);
      TEST_CHECK(service.error() && service.error()->kind == files::FileErrorKind::Unsupported);
      TEST_CHECK(service.error()->message.contains("decoded-container") && !fs::exists(output));
      TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
    }
  }
  std::string xz(std::string_view bytes) {
    std::string encoded(lzma_stream_buffer_bound(bytes.size()), '\0');
    std::size_t position = 0;
    TEST_CHECK(
        lzma_easy_buffer_encode(
            0, LZMA_CHECK_CRC64, nullptr, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
            reinterpret_cast<std::uint8_t*>(encoded.data()), &position, encoded.size()
        )
        == LZMA_OK
    );
    encoded.resize(position);
    return encoded;
  }
  std::string oversizedXzDictionary(std::string encoded) {
    TEST_CHECK(encoded.size() > 16);
    lzma_block block{};
    std::array<lzma_filter, LZMA_FILTERS_MAX + 1> filters{};
    block.version = 0;
    block.filters = filters.data();
    block.header_size = lzma_block_header_size_decode(static_cast<unsigned char>(encoded[12]));
    TEST_CHECK(block.header_size <= encoded.size() - 12);
    TEST_CHECK(
        lzma_block_header_decode(&block, nullptr, reinterpret_cast<const std::uint8_t*>(encoded.data() + 12)) == LZMA_OK
    );
    TEST_CHECK(filters[0].id == LZMA_FILTER_LZMA2 && filters[1].id == LZMA_VLI_UNKNOWN);
    auto* options = static_cast<lzma_options_lzma*>(filters[0].options);
    options->dict_size = 256U * 1024U * 1024U;
    TEST_CHECK(lzma_raw_decoder_memusage(filters.data()) > 128U * 1024U * 1024U);
    TEST_CHECK(lzma_block_header_encode(&block, reinterpret_cast<std::uint8_t*>(encoded.data() + 12)) == LZMA_OK);
    lzma_filters_free(filters.data(), nullptr);
    return encoded;
  }
  void testXzTar(const Fixture& fixture, Pump& pump) {
    TEST_CHECK(files::archiveCompressionAvailable(files::ArchiveCompression::Xz));
    const auto formats = files::archiveFormats();
    const auto format = std::ranges::find(formats, files::ArchiveFormat::TarXz, &files::ArchiveFormatInfo::format);
    TEST_CHECK((format != formats.end()) == (archive_liblzma_version() != nullptr));
    if (format != formats.end()) {
      TEST_CHECK(format->compression == files::ArchiveCompression::Xz && format->suffix == ".tar.xz");
    }
    const auto plain = fixture.root / "xz-source.tar";
    tar(plain, {{"file", "XZ payload", {}}, {"alias", {}, "file", AE_IFREG}, {"link", {}, "file", AE_IFLNK}});
    const auto data = read(plain);
    const auto encoded = xz(data);
    if (format != formats.end()) {
      const auto source = fixture.root / "xz-writer-payload";
      const auto archive = fixture.root / "xz-writer.tar.xz";
      write(source, "independent reader");
      files::ArchiveService service;
      TEST_CHECK(!service.compress({location(source)}, location(archive), format->format, "unsupported"));
      TEST_CHECK(service.compress({location(source)}, location(archive), format->format));
      wait(service, pump);
      TEST_CHECK(!service.error());
      const std::unique_ptr<struct archive, decltype(&archive_read_free)> reader(
          archive_read_new(), &archive_read_free
      );
      TEST_CHECK(archive_read_support_filter_xz(reader.get()) == ARCHIVE_OK);
      TEST_CHECK(archive_read_support_format_tar(reader.get()) == ARCHIVE_OK);
      TEST_CHECK(archive_read_open_filename(reader.get(), archive.c_str(), 65536) == ARCHIVE_OK);
      struct archive_entry* header = nullptr;
      TEST_CHECK(archive_read_next_header(reader.get(), &header) == ARCHIVE_OK);
      TEST_CHECK(archive_filter_code(reader.get(), 0) == ARCHIVE_FILTER_XZ);
      std::array<char, 64> bytes{};
      const auto count = archive_read_data(reader.get(), bytes.data(), bytes.size());
      TEST_CHECK(count == 18 && std::string_view(bytes.data(), static_cast<std::size_t>(count)) == read(source));
    }
    for (unsigned variant = 0; variant < 4; ++variant) {
      const auto source = fixture.root / ("independent-xz-" + std::to_string(variant) + ".txz");
      // Independent encoder, legal padding and multiple streams, including a
      // frame boundary inside a TAR header rather than between TAR members.
      const auto bytes = variant == 0 ? encoded
          : variant == 1              ? encoded + std::string(4, '\0')
          : variant == 2 ? xz(std::string_view(data).substr(0, 700)) + xz(std::string_view(data).substr(700))
                         : xz({}) + encoded + xz({});
      write(source, bytes);
      files::ArchiveService service;
      TEST_CHECK(service.inspect(location(source)));
      wait(service, pump);
      TEST_CHECK(!service.error() && service.catalog() && service.catalog()->entries.size() == 3);
      const auto known = service.catalog();
      const auto output = fixture.root / ("xz-extracted-" + std::to_string(variant));
      TEST_CHECK(service.extract(location(source), location(output), {}, known));
      wait(service, pump);
      TEST_CHECK(!service.error() && read(output / "file") == "XZ payload" && read(output / "alias") == "XZ payload");
      TEST_CHECK(fs::is_symlink(output / "link") && fs::read_symlink(output / "link") == "file");
      const auto selected = fixture.root / ("xz-selected-" + std::to_string(variant));
      TEST_CHECK(service.extract(location(source), location(selected), {"alias"}, known));
      wait(service, pump);
      TEST_CHECK(!service.error() && read(selected / "alias") == "XZ payload" && !fs::exists(selected / "file"));
      write(source, bytes);
      const auto stale = fixture.root / ("xz-stale-" + std::to_string(variant));
      TEST_CHECK(service.extract(location(source), location(stale), {}, known));
      wait(service, pump);
      TEST_CHECK(service.error() && service.error()->kind == files::FileErrorKind::Changed && !fs::exists(stale));
    }
  }
  void testXzFailures(const Fixture& fixture, Pump& pump) {
    const auto plain = fixture.root / "xz-boundary.tar";
    tar(plain, {{"payload", std::string(129536, 'x'), {}}});
    const auto data = read(plain);
    const auto valid = xz(data);
    const auto hostile = fixture.root / "xz-hostile.tar";
    tar(hostile, {{"../escaped-xz", "bad", {}}});
    const auto link = fixture.root / "xz-hostile-link.tar";
    tar(link, {{"link", {}, "../escaped-xz", AE_IFLNK}});
    std::vector<std::string> rejected{
        valid.substr(0, valid.size() - 1),
        valid.substr(0, valid.size() / 2),
        valid + std::string(1, '\0'),
        valid + "garbage",
        oversizedXzDictionary(valid),
        xz(data + std::string(4U * 1024U * 1024U, '\0')),
        xz(read(hostile)),
        xz(read(link)),
        valid
    };
    auto badCheck = valid;
    badCheck[badCheck.size() - 13] ^= 0x01;
    rejected.push_back(std::move(badCheck));
    std::uint32_t backward = 0;
    for (unsigned i = 0; i < 4; ++i) {
      backward |= static_cast<std::uint32_t>(static_cast<unsigned char>(valid[valid.size() - 8 + i])) << (8 * i);
    }
    auto badDataCheck = valid;
    const auto indexSize = (static_cast<std::size_t>(backward) + 1) * 4;
    TEST_CHECK(indexSize + 13 < valid.size());
    badDataCheck[valid.size() - indexSize - 13] ^= 0x01;
    rejected.push_back(std::move(badDataCheck));
    rejected.push_back(valid + oversizedXzDictionary(valid));
    rejected.push_back(valid + valid.substr(0, valid.size() - 1));
    for (std::size_t i = 0; i < rejected.size(); ++i) {
      const auto source = fixture.root / ("bad-xz-" + std::to_string(i) + ".tar.xz");
      write(source, rejected[i]);
      files::ArchiveLimits limits;
      if (i == 5) {
        limits.maxExpandedBytes = data.size();
        limits.maxMetadataBytes = 256;
        limits.maxEntries = 4;
      } else if (i == 8) {
        limits.maxExpandedBytes = 1024;
      }
      files::ArchiveService service(limits);
      TEST_CHECK(service.inspect(location(source)));
      wait(service, pump);
      TEST_CHECK(service.error() && !service.catalog());
      if (i == 4 || i == 5) {
        TEST_CHECK(service.error()->kind == files::FileErrorKind::Unsupported);
        TEST_CHECK(service.error()->message.contains(i == 4 ? "128 MiB" : "decoded-container"));
      }
      const auto destination = fixture.root / ("xz-rejected-" + std::to_string(i));
      TEST_CHECK(service.extract(location(source), location(destination)));
      wait(service, pump);
      TEST_CHECK(service.error() && !fs::exists(destination) && fs::is_empty(fixture.root / "tmp"));
    }
    TEST_CHECK(!fs::exists(fixture.root / "escaped-xz"));
    const auto split = splitZip(fixture.root / "xz-fake-split", "fake.zip", valid, valid.size() / 2);
    files::ArchiveService splitService;
    TEST_CHECK(splitService.inspect(location(split.front())));
    wait(splitService, pump);
    TEST_CHECK(splitService.error() && !splitService.catalog());
    const auto source = fixture.root / "xz-cancel.tar.xz";
    write(source, valid);
    files::ArchiveService service;
    TEST_CHECK(service.extract(location(source), location(fixture.root / "xz-cancelled")));
    service.cancel();
    wait(service, pump);
    TEST_CHECK(service.error() && !fs::exists(fixture.root / "xz-cancelled") && fs::is_empty(fixture.root / "tmp"));
  }
  void testXzMemberFromLargeArchive(const Fixture& fixture, Pump& pump) {
    if (!archive_liblzma_version()) {
      return;
    }
    const auto source = fixture.root / "large-xz.tar.xz";
    const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(
        archive_write_new(), &archive_write_free
    );
    TEST_CHECK(archive_write_set_format_pax_restricted(writer.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_add_filter_xz(writer.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_set_filter_option(writer.get(), "xz", "compression-level", "0") == ARCHIVE_OK);
    TEST_CHECK(archive_write_set_bytes_per_block(writer.get(), 0) == ARCHIVE_OK);
    TEST_CHECK(archive_write_open_filename(writer.get(), source.c_str()) == ARCHIVE_OK);
    const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
        archive_entry_new(), &archive_entry_free
    );
    archive_entry_set_pathname(entry.get(), "large");
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_size(entry.get(), 130U * 1024U * 1024U);
    TEST_CHECK(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    const std::array<char, 65536> chunk{};
    for (unsigned i = 0; i < 130U * 16U; ++i) {
      TEST_CHECK(archive_write_data(writer.get(), chunk.data(), chunk.size()) == static_cast<la_ssize_t>(chunk.size()));
    }
    archive_entry_set_pathname(entry.get(), "small");
    archive_entry_set_size(entry.get(), 3);
    TEST_CHECK(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
    TEST_CHECK(archive_write_data(writer.get(), "abc", 3) == 3);
    TEST_CHECK(archive_write_close(writer.get()) == ARCHIVE_OK);
    files::ArchiveLimits limits;
    limits.maxExpandedBytes = 256U * 1024U * 1024U;
    limits.maxMetadataBytes = 256;
    limits.maxEntries = 4;
    files::ArchiveService service(limits);
    TEST_CHECK(service.inspect(location(source)));
    wait(service, pump);
    TEST_CHECK(!service.error() && service.catalog());
    TEST_CHECK(service.openMember("small"));
    wait(service, pump);
    const auto opened = service.takeOpenedFile();
    TEST_CHECK(!service.error() && opened && read(*opened->nativePath()) == "abc");
    service.shutdown();
    wait(service, pump);
    TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
  }
  void testXzWorkerBoundaries(const Fixture& fixture) {
    const auto plain = fixture.root / "xz-worker.tar";
    tar(plain, {{"file", std::string(256U * 1024U, 'x'), {}}});
    const auto source = fixture.root / "xz-worker.tar.xz";
    write(source, xz(read(plain)));
    files::ArchiveLimits limits;
    files::CancelToken cancel;
    files::archive_work::Progress progress;
    auto work = files::archive_work::workFor(limits, cancel, progress);
    files::archive_work::Input input(location(source), work);
    TEST_CHECK(files::archive_work::XzInput::recognizes(input));
    files::archive_work::XzInput decoder(input, work);
    std::array<char, 512> bytes{};
    TEST_CHECK(decoder.read(bytes) == bytes.size());
    cancel.cancel();
    bool cancelled = false;
    try {
      decoder.finish();
    } catch (const files::FileErrorException& error) {
      cancelled = error.error().kind == files::FileErrorKind::Cancelled;
    }
    TEST_CHECK(cancelled);
  }
  int compressedWithoutTools() {
    Fixture fixture;
    Pump pump;
    const auto source = fixture.root / "payload";
    write(source, "no helper program");
    for (const auto& item : files::archiveFormats()) {
      const auto archive = fixture.root / ("no-tools" + std::string(item.suffix));
      const auto destination = fixture.root / ("no-tools" + std::to_string(static_cast<int>(item.format)));
      files::ArchiveService service;
      TEST_CHECK(service.compress({location(source)}, location(archive), item.format));
      wait(service, pump);
      TEST_CHECK(!service.error());
      TEST_CHECK(service.extract(location(archive), location(destination)));
      wait(service, pump);
      TEST_CHECK(!service.error() && read(destination / "payload") == "no helper program");
    }
    return 0;
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
    for (const auto& format : files::archiveFormats()) {
      files::archive_work::Progress progress;
      files::archive_work::Passwords passwords;
      if (format.format == files::ArchiveFormat::Zip) {
        passwords.values.emplace_back("test password");
      }
      bool failed = false;
      try {
        auto prepared = files::archive_work::compress(
            {location(source)}, location(fixture.root / "failed-output"), format.format, {}, cancel, progress, passwords
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
    if (argc == 2 && std::string_view(argv[1]) == "--compressed-without-tools") {
      return compressedWithoutTools();
    }
    Fixture fixture;
    Pump pump;
    testXzTar(fixture, pump);
    testXzFailures(fixture, pump);
    testXzWorkerBoundaries(fixture);
    testXzMemberFromLargeArchive(fixture, pump);
    testCompressedTarProviders(fixture, pump);
    testCompressedTarFailures(fixture, pump);
    testCompressedPaddingLimit(fixture, pump);
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
    testSplitInput(fixture);
    testSplitZip(fixture, pump);
    testSplitFailures(fixture, pump);
    testSplitPasswordAndChanges(fixture, pump);
    testCancelAndDestruction(fixture, pump);
    files::GioPtr<GSubprocess> child(
        g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, nullptr, "/proc/self/exe", "--bounded-write-failure", nullptr)
    );
    TEST_CHECK(child && g_subprocess_wait_check(child.get(), nullptr, nullptr));
    const files::GioPtr<GSubprocessLauncher> launcher(g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE));
    g_subprocess_launcher_setenv(launcher.get(), "PATH", "/noctalia-files-no-helper-tools", TRUE);
    files::GioPtr<GSubprocess> native(
        g_subprocess_launcher_spawn(launcher.get(), nullptr, "/proc/self/exe", "--compressed-without-tools", nullptr)
    );
    TEST_CHECK(native && g_subprocess_wait_check(native.get(), nullptr, nullptr));
    TEST_CHECK(fs::is_empty(fixture.root / "tmp"));
    std::println("archive checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "archive test: {}", error.what());
    return 1;
  }
}
