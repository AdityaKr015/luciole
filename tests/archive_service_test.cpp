#include "app/glib_poll_source.h"
#include "operations/archive_service.h"
#include "operations/archive_work.h"

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
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  struct Fixture {
    fs::path root;
    Fixture() {
      std::string pattern = "/tmp/noctalia-archive-test-XXXXXX";
      const char* path = ::mkdtemp(pattern.data());
      require(path, "cannot create archive fixture");
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
        require(std::chrono::steady_clock::now() < deadline, "archive operation timed out");
        std::vector<pollfd> fds;
        auto start = m_source.addPollFds(fds);
        const int timeout = m_source.pollTimeoutMs();
        require(::poll(fds.data(), fds.size(), timeout < 0 ? 20 : std::min(timeout, 20)) >= 0, "poll failed");
        m_source.dispatch(fds, start);
      }
    }

  private:
    GlibPollSource m_source;
  };
  void write(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    require(out.good(), "cannot write archive fixture");
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
    require(archive_write_set_format_pax_restricted(writer.get()) == ARCHIVE_OK, "cannot configure tar fixture");
    require(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK, "cannot open tar fixture");
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
        if (item.kind == AE_IFLNK)
          archive_entry_set_symlink(entry.get(), item.link.c_str());
        else
          archive_entry_set_hardlink(entry.get(), item.link.c_str());
      }
      require(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK, "cannot write hostile tar header");
      if (item.kind == AE_IFREG && item.link.empty() && !item.data.empty())
        require(
            archive_write_data(writer.get(), item.data.data(), item.data.size())
                == static_cast<la_ssize_t>(item.data.size()),
            "cannot write tar fixture data"
        );
    }
    require(archive_write_close(writer.get()) == ARCHIVE_OK, "cannot finish tar fixture");
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
    require(!files::archiveFormats().empty(), "archive backend has no write formats");
    for (const auto& format : files::archiveFormats()) {
      const auto archive = fixture.root / ("roundtrip" + std::string(format.suffix));
      files::ArchiveService service;
      require(service.compress({location(src)}, location(archive), format.format), "compression was not accepted");
      wait(service, pump);
      if (!service.error().empty())
        throw std::runtime_error(service.error());
      require(service.error().empty() && fs::exists(archive), "compression failed");
      require(service.inspect(location(archive)), "inspection was not accepted");
      wait(service, pump);
      require(
          service.error().empty() && service.catalog() && service.catalog()->entries.size() >= 5,
          "archive contents missing"
      );
      const auto dest = fixture.root / ("unpacked" + std::to_string(static_cast<int>(format.format)));
      require(service.extract(location(archive), location(dest)), "extraction was not accepted");
      wait(service, pump);
      require(
          service.error().empty()
              && read(dest / "source/hello.txt") == "archive content"
              && read(dest / "source/café\nnotes.txt") == "Unicode and newline"
              && fs::is_directory(dest / "source/empty"),
          "archive round-trip changed files"
      );
      require(
          fs::is_symlink(dest / "source/link") && fs::read_symlink(dest / "source/link") == "hello.txt",
          "safe symlink was not retained"
      );
      require(service.openMember("source/hello.txt"), "member open was not accepted");
      wait(service, pump);
      const auto opened = service.takeOpenedFile();
      require(
          opened && read(*opened->nativePath()) == "archive content", "member open did not materialize its own data"
      );
      const auto temporary = *opened->nativePath();
      service.shutdown();
      wait(service, pump);
      require(!fs::exists(temporary), "temporary open files survived shutdown");
    }
  }
  void testConflicts(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "conflict.txt", archive = fixture.root / "conflict.zip";
    write(source, "first");
    files::ArchiveService service;
    require(
        service.compress({location(source)}, location(archive), files::ArchiveFormat::ZIP),
        "cannot start collision fixture"
    );
    wait(service, pump);
    const auto original = read(archive);
    write(source, "second content");
    for (const auto choice :
         {files::TransferConflictChoice::SKIP, files::TransferConflictChoice::KeepBoth,
          files::TransferConflictChoice::REPLACE}) {
      require(
          service.compress({location(source)}, location(archive), files::ArchiveFormat::ZIP),
          "cannot retry archive publication"
      );
      pump.until([&] { return service.conflict().has_value(); });
      const auto conflict = service.conflict();
      require(service.resolve(conflict->id, choice), "archive conflict decision rejected");
      wait(service, pump);
      if (choice != files::TransferConflictChoice::REPLACE)
        require(read(archive) == original, "unapproved conflict overwrote archive");
      else
        require(service.error().empty() && read(archive) != original, "approved replacement failed");
    }
    require(fs::exists(fixture.root / "conflict (1).zip"), "keep-both naming was not reused");
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
      require(service.extract(location(source), location(dest)), "hostile fixture was not attempted");
      wait(service, pump);
      require(!service.error().empty() && !fs::exists(dest), "hostile archive changed its destination");
    }
    require(!fs::exists(fixture.root / "escaped"), "archive traversed the destination boundary");
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
      require(service.extract(location(source), location(dest)), "limit fixture was not attempted");
      wait(service, pump);
      require(!service.error().empty() && !fs::exists(dest), "archive bypassed a resource limit");
    }
    const auto truncated = fixture.root / "truncated.tar";
    fs::copy_file(source, truncated);
    fs::resize_file(truncated, 515);
    files::ArchiveService service;
    const auto dest = fixture.root / "truncated-output";
    require(service.extract(location(truncated), location(dest)), "truncated archive not attempted");
    wait(service, pump);
    require(!service.error().empty() && !fs::exists(dest), "truncated data was published");
  }
  void testLinksAndSelection(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "links.tar";
    tar(source,
        {{"dir/real", "linked data", {}, AE_IFREG, 04777},
         {"alias", {}, "dir/real", AE_IFREG},
         {"safe", {}, "dir/../dir/real", AE_IFLNK}});
    files::ArchiveService service;
    require(service.inspect(location(source)), "cannot inspect linked fixture");
    wait(service, pump);
    require(service.error().empty(), "valid linked fixture rejected");
    const auto dest = fixture.root / "selected";
    require(service.extract(location(source), location(dest), {"alias"}), "selected link extraction not attempted");
    wait(service, pump);
    require(
        service.error().empty() && read(dest / "alias") == "linked data" && !fs::exists(dest / "dir/real"),
        "selected hard link did not materialize its own content"
    );
    struct stat info{};
    require(
        ::stat((dest / "alias").c_str(), &info) == 0 && (info.st_mode & S_ISUID) == 0 && info.st_uid == ::getuid(),
        "privileged archive attributes were restored"
    );
    write(source, read(source) + "changed");
    require(
        service.extract(location(source), location(fixture.root / "changed"), {"alias"}),
        "changed fixture not attempted"
    );
    wait(service, pump);
    require(!service.error().empty() && !fs::exists(fixture.root / "changed"), "changed catalog authorized extraction");
    require(
        service.extract(location(source), location(fixture.root / "changed-all")),
        "changed whole-archive fixture not attempted"
    );
    wait(service, pump);
    require(
        !service.error().empty() && !fs::exists(fixture.root / "changed-all"),
        "whole-archive extraction ignored a changed catalog"
    );
  }
  void testHardlinkOutputLimit(const Fixture& fixture, Pump& pump) {
    const auto path = fixture.root / "link-limit.tar";
    tar(path, {{"body", "12345", {}}, {"alias", {}, "body"}});
    files::ArchiveLimits limits;
    limits.maxExpandedBytes = 9;
    files::ArchiveService service(limits);
    require(service.extract(location(path), location(fixture.root / "link-limit")), "hardlink limit did not start");
    wait(service, pump);
    require(
        !service.error().empty() && !fs::exists(fixture.root / "link-limit"), "hardlink aliases bypassed output limit"
    );
  }
  void testEncryptedAndWriteFailure(const Fixture& fixture, Pump& pump) {
    const auto path = fixture.root / "encrypted.zip";
    const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(
        archive_write_new(), &archive_write_free
    );
    require(
        archive_write_set_format_zip(writer.get()) == ARCHIVE_OK
            && archive_write_set_options(writer.get(), "zip:encryption=aes256") == ARCHIVE_OK
            && archive_write_set_passphrase(writer.get(), "test password") == ARCHIVE_OK,
        "cannot configure encrypted fixture"
    );
    require(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK, "cannot open encrypted fixture");
    const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
        archive_entry_new(), &archive_entry_free
    );
    archive_entry_set_pathname(entry.get(), "secret.txt");
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0600);
    archive_entry_set_size(entry.get(), 6);
    require(
        archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK
            && archive_write_data(writer.get(), "secret", 6) == 6
            && archive_write_close(writer.get()) == ARCHIVE_OK,
        "cannot create encrypted fixture"
    );
    files::ArchiveService service;
    require(
        service.extract(location(path), location(fixture.root / "encrypted-output")),
        "encrypted extraction did not start"
    );
    wait(service, pump);
    require(
        !service.error().empty() && !fs::exists(fixture.root / "encrypted-output"), "encrypted input published output"
    );
    const auto file = fixture.root / "not-a-folder";
    write(file, "untouched");
    require(
        service.compress({location(file)}, location(file / "failure.tar"), files::ArchiveFormat::TAR),
        "invalid destination did not reach worker"
    );
    wait(service, pump);
    require(!service.error().empty() && read(file) == "untouched", "publication failure changed existing destination");
  }
  int boundedWriteFailure() {
    Fixture fixture;
    const auto source = fixture.root / "large.txt";
    write(source, std::string(8192, 'a'));
    std::signal(SIGXFSZ, SIG_IGN);
    const rlimit limit{1024, 1024};
    require(::setrlimit(RLIMIT_FSIZE, &limit) == 0, "cannot limit fixture output");
    files::archive_work::Progress progress;
    files::GioPtr<GCancellable> cancel(g_cancellable_new());
    bool failed = false;
    try {
      auto prepared = files::archive_work::compress(
          {location(source)}, location(fixture.root / "out.tar"), files::ArchiveFormat::TAR, {}, cancel.get(), progress
      );
      (void)files::archive_work::cleanup({prepared.stage});
    } catch (const std::exception&) {
      failed = true;
    }
    require(
        failed && !fs::exists(fixture.root / "out.tar") && fs::is_empty(fixture.root / "tmp"),
        "bounded output failure leaked or published data"
    );
    return 0;
  }
  void testCancelAndDestruction(const Fixture& fixture, Pump& pump) {
    const auto source = fixture.root / "cancel.txt", output = fixture.root / "cancel.zip";
    write(source, std::string(1024 * 1024, 'x'));
    files::ArchiveService service;
    require(
        service.compress({location(source)}, location(output), files::ArchiveFormat::ZIP),
        "cancel fixture did not start"
    );
    service.cancel();
    wait(service, pump);
    require(!fs::exists(output) && !service.error().empty(), "cancelled compression was published");
    write(output, "keep me");
    auto owner = std::make_unique<files::ArchiveService>();
    require(
        owner->compress({location(source)}, location(output), files::ArchiveFormat::ZIP),
        "destruction fixture did not start"
    );
    pump.until([&] { return owner->conflict().has_value(); });
    owner.reset();
    pump.until([&] { return fs::is_empty(fixture.root / "tmp"); });
    require(read(output) == "keep me", "destroying a pending archive job changed its destination");
  }
} // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--bounded-write-failure")
      return boundedWriteFailure();
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
    require(child && g_subprocess_wait_check(child.get(), nullptr, nullptr), "bounded output subprocess failed");
    require(fs::is_empty(fixture.root / "tmp"), "archive staging leaked");
    std::println("archive checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "archive test: {}", error.what());
    return 1;
  }
}
