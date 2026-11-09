#include "event_loop/glib_poll_source.h"
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
      if (!service.error().empty()) {
        throw std::runtime_error(service.error());
      }
      TEST_CHECK(service.error().empty() && fs::exists(archive));
      TEST_CHECK(service.inspect(location(archive)));
      wait(service, pump);
      TEST_CHECK(service.error().empty() && service.catalog() && service.catalog()->entries.size() >= 5);
      const auto dest = fixture.root / ("unpacked" + std::to_string(static_cast<int>(format.format)));
      TEST_CHECK(service.extract(location(archive), location(dest)));
      wait(service, pump);
      TEST_CHECK(
          service.error().empty()
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
        TEST_CHECK(service.error().empty() && read(archive) != original);
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
      TEST_CHECK(!service.error().empty() && !fs::exists(dest));
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
      TEST_CHECK(!service.error().empty() && !fs::exists(dest));
    }
    const auto truncated = fixture.root / "truncated.tar";
    fs::copy_file(source, truncated);
    fs::resize_file(truncated, 515);
    files::ArchiveService service;
    const auto dest = fixture.root / "truncated-output";
    TEST_CHECK(service.extract(location(truncated), location(dest)));
    wait(service, pump);
    TEST_CHECK(!service.error().empty() && !fs::exists(dest));
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
    TEST_CHECK(service.error().empty());
    const auto dest = fixture.root / "selected";
    TEST_CHECK(service.extract(location(source), location(dest), {"alias"}));
    wait(service, pump);
    TEST_CHECK(service.error().empty() && read(dest / "alias") == "linked data" && !fs::exists(dest / "dir/real"));
    struct stat info{};
    TEST_CHECK(
        ::stat((dest / "alias").c_str(), &info) == 0 && (info.st_mode & S_ISUID) == 0 && info.st_uid == ::getuid()
    );
    write(source, read(source) + "changed");
    TEST_CHECK(service.extract(location(source), location(fixture.root / "changed"), {"alias"}));
    wait(service, pump);
    TEST_CHECK(!service.error().empty() && !fs::exists(fixture.root / "changed"));
    TEST_CHECK(service.extract(location(source), location(fixture.root / "changed-all")));
    wait(service, pump);
    TEST_CHECK(!service.error().empty() && !fs::exists(fixture.root / "changed-all"));
  }
  void testHardlinkOutputLimit(const Fixture& fixture, Pump& pump) {
    const auto path = fixture.root / "link-limit.tar";
    tar(path, {{"body", "12345", {}}, {"alias", {}, "body"}});
    files::ArchiveLimits limits;
    limits.maxExpandedBytes = 9;
    files::ArchiveService service(limits);
    TEST_CHECK(service.extract(location(path), location(fixture.root / "link-limit")));
    wait(service, pump);
    TEST_CHECK(!service.error().empty() && !fs::exists(fixture.root / "link-limit"));
  }
  void testEncryptedAndWriteFailure(const Fixture& fixture, Pump& pump) {
    const auto path = fixture.root / "encrypted.zip";
    const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(
        archive_write_new(), &archive_write_free
    );
    TEST_CHECK(
        archive_write_set_format_zip(writer.get()) == ARCHIVE_OK
        && archive_write_set_options(writer.get(), "zip:encryption=aes256") == ARCHIVE_OK
        && archive_write_set_passphrase(writer.get(), "test password") == ARCHIVE_OK
    );
    TEST_CHECK(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK);
    const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
        archive_entry_new(), &archive_entry_free
    );
    archive_entry_set_pathname(entry.get(), "secret.txt");
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0600);
    archive_entry_set_size(entry.get(), 6);
    TEST_CHECK(
        archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK
        && archive_write_data(writer.get(), "secret", 6) == 6
        && archive_write_close(writer.get()) == ARCHIVE_OK
    );
    files::ArchiveService service;
    TEST_CHECK(service.extract(location(path), location(fixture.root / "encrypted-output")));
    wait(service, pump);
    TEST_CHECK(!service.error().empty() && !fs::exists(fixture.root / "encrypted-output"));
    const auto file = fixture.root / "not-a-folder";
    write(file, "untouched");
    TEST_CHECK(service.compress({location(file)}, location(file / "failure.tar"), files::ArchiveFormat::Tar));
    wait(service, pump);
    TEST_CHECK(!service.error().empty() && read(file) == "untouched");
  }
  int boundedWriteFailure() {
    Fixture fixture;
    const auto source = fixture.root / "large.txt";
    write(source, std::string(8192, 'a'));
    std::signal(SIGXFSZ, SIG_IGN);
    const rlimit limit{1024, 1024};
    TEST_CHECK(::setrlimit(RLIMIT_FSIZE, &limit) == 0);
    files::archive_work::Progress progress;
    files::GioPtr<GCancellable> cancel(g_cancellable_new());
    bool failed = false;
    try {
      auto prepared = files::archive_work::compress(
          {location(source)}, location(fixture.root / "out.tar"), files::ArchiveFormat::Tar, {}, cancel.get(), progress
      );
      (void)files::archive_work::cleanup({prepared.stage});
    } catch (const std::exception&) {
      failed = true;
    }
    TEST_CHECK(failed && !fs::exists(fixture.root / "out.tar") && fs::is_empty(fixture.root / "tmp"));
    return 0;
  }
  void testCancelAndDestruction(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "cancel.txt", output = fixture.root / "cancel.zip";
    write(source, std::string(1024 * 1024, 'x'));
    files::ArchiveService service;
    TEST_CHECK(service.compress({location(source)}, location(output), files::ArchiveFormat::Zip));
    service.cancel();
    wait(service, pump);
    TEST_CHECK(!fs::exists(output) && !service.error().empty());
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
    testEncryptedAndWriteFailure(fixture, pump);
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
