#include "app/glib_poll_source.h"
#include "browser/directory_model.h"
#include "operations/create_service.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  files::Location location(const fs::path& path) { return files::Location::fromPath(path.native()); }
  struct Fixture {
    fs::path root;
    Fixture() {
      std::string path = "/tmp/noctalia-create-XXXXXX";
      TEST_CHECK(::mkdtemp(path.data()) != nullptr);
      root = path;
    }
    ~Fixture() {
      std::error_code error;
      fs::permissions(root, fs::perms::owner_all, fs::perm_options::add, error);
      fs::remove_all(root, error);
    }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto first = source.addPollFds(fds);
        TEST_CHECK(::poll(fds.data(), fds.size(), 1) >= 0);
        source.dispatch(fds, first);
      }
    }
    void
    prepare(files::CreateService& service, const fs::path& path, files::CreateKind kind = files::CreateKind::Folder) {
      TEST_CHECK(service.prepare(location(path), kind));
      until([&] { return !service.busy(); });
      TEST_CHECK(service.canCreate());
    }
    void create(files::CreateService& service, const std::string& name) {
      TEST_CHECK(service.create(name));
      until([&] { return !service.busy(); });
    }

  private:
    GlibPollSource source;
  };
  void createsFolders(Pump& pump) {
    Fixture fixture;
    files::CreateService service;
    pump.prepare(service, fixture.root);
    pump.create(service, "Documents 文");
    TEST_CHECK(
        service.snapshot().status == files::CreateStatus::Succeeded && fs::is_empty(fixture.root / "Documents 文")
    );
    TEST_CHECK(service.snapshot().destination == location(fixture.root / "Documents 文"));
    pump.prepare(service, fixture.root);
    pump.create(service, ".hidden");
    TEST_CHECK(fs::is_directory(fixture.root / ".hidden"));
    const auto raw = std::string("raw-") + static_cast<char>(0xff);
    fs::create_directory(fixture.root / raw);
    pump.prepare(service, fixture.root / raw);
    pump.create(service, "child");
    TEST_CHECK(fs::is_directory(fixture.root / raw / "child"));
    fs::create_directory_symlink(fixture.root, fixture.root / "alias");
    pump.prepare(service, fixture.root / "alias");
    pump.create(service, "through link");
    TEST_CHECK(fs::is_directory(fixture.root / "through link"));
  }
  void collisionsAndRetry(Pump& pump) {
    Fixture fixture;
    fs::create_directory(fixture.root / "folder");
    std::ofstream(fixture.root / "folder/marker") << "preserve";
    std::ofstream(fixture.root / "file") << "data";
    fs::create_symlink("missing", fixture.root / "link");
    files::CreateService service;
    pump.prepare(service, fixture.root);
    for (const auto* name : {"folder", "file", "link"}) {
      pump.create(service, name);
      TEST_CHECK(service.snapshot().status == files::CreateStatus::Error && service.canCreate());
    }
    TEST_CHECK(
        fs::exists(fixture.root / "folder/marker")
        && fs::is_regular_file(fixture.root / "file")
        && fs::read_symlink(fixture.root / "link") == "missing"
    );
    pump.create(service, "available");
    TEST_CHECK(service.snapshot().status == files::CreateStatus::Succeeded);
    files::CreateService other;
    pump.prepare(service, fixture.root);
    pump.prepare(other, fixture.root);
    TEST_CHECK(service.create("racing") && other.create("racing"));
    pump.until([&] { return !service.busy() && !other.busy(); });
    const bool first = service.snapshot().status == files::CreateStatus::Succeeded;
    const bool second = other.snapshot().status == files::CreateStatus::Succeeded;
    TEST_CHECK(first != second && fs::is_directory(fixture.root / "racing"));
  }
  void staleDestinations(Pump& pump, files::CreateKind kind) {
    Fixture fixture;
    fs::create_directory(fixture.root / "parent");
    files::CreateService service;
    pump.prepare(service, fixture.root / "parent", kind);
    fs::rename(fixture.root / "parent", fixture.root / "original");
    fs::create_directory(fixture.root / "parent");
    pump.create(service, "blocked");
    TEST_CHECK(
        service.snapshot().status == files::CreateStatus::Error
        && !fs::exists(fixture.root / "parent/blocked")
        && !fs::exists(fixture.root / "original/blocked")
    );
    fs::create_directory_symlink(fixture.root / "parent", fixture.root / "alias");
    pump.prepare(service, fixture.root / "alias", kind);
    fs::remove(fixture.root / "alias");
    fs::create_directory_symlink(fixture.root / "original", fixture.root / "alias");
    pump.create(service, "blocked");
    TEST_CHECK(service.snapshot().status == files::CreateStatus::Error && fs::is_empty(fixture.root / "original"));
  }
  void validationCancellationAndPermissions(Pump& pump) {
    Fixture fixture;
    files::CreateService service;
    pump.prepare(service, fixture.root);
    for (const auto& name : std::vector<std::string>{
             "", ".", "..", "nested/name", std::string("a\0b", 3), std::string(1, static_cast<char>(0xff))
         }) {
      TEST_CHECK(!service.create(name) && fs::is_empty(fixture.root));
    }
    pump.create(service, std::string(10000, 'a'));
    TEST_CHECK(service.snapshot().status == files::CreateStatus::Error && fs::is_empty(fixture.root));
    service.cancel();
    TEST_CHECK(!service.canCreate() && fs::is_empty(fixture.root));
    TEST_CHECK(service.prepare(location(fixture.root), files::CreateKind::Folder));
    TEST_CHECK(!service.prepare(location(fixture.root), files::CreateKind::Folder) && !service.create("busy"));
    service.cancel();
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == files::CreateStatus::Cancelled && fs::is_empty(fixture.root));
    pump.prepare(service, fixture.root);
    TEST_CHECK(service.create("maybe-created"));
    service.cancel();
    pump.until([&] { return !service.busy(); });
    const bool committed = service.snapshot().status == files::CreateStatus::Succeeded;
    TEST_CHECK(
        (committed || service.snapshot().status == files::CreateStatus::Cancelled)
        && fs::exists(fixture.root / "maybe-created") == committed
    );
    for (const auto& parent :
         {location(fixture.root / "missing"), files::Location::fromUri("sftp://example.invalid/folder")}) {
      TEST_CHECK(service.prepare(parent, files::CreateKind::Folder));
      pump.until([&] { return !service.busy(); });
      TEST_CHECK(service.snapshot().status == files::CreateStatus::Error && !service.canCreate());
    }
    if (::geteuid() != 0) {
      pump.prepare(service, fixture.root);
      fs::permissions(fixture.root, fs::perms::owner_read | fs::perms::owner_exec);
      pump.create(service, "denied");
      fs::permissions(fixture.root, fs::perms::owner_all);
      TEST_CHECK(service.snapshot().status == files::CreateStatus::Error && !fs::exists(fixture.root / "denied"));
    }
    struct MaskGuard {
      mode_t previous = ::umask(0027);
      ~MaskGuard() { ::umask(previous); }
    } guard;
    pump.prepare(service, fixture.root);
    pump.create(service, "permissions");
    struct stat info{};
    TEST_CHECK(::stat((fixture.root / "permissions").c_str(), &info) == 0 && (info.st_mode & 0777) == 0750);
  }
  void createsEmptyFiles(Pump& pump) {
    Fixture fixture;
    files::CreateService service;
    pump.prepare(service, fixture.root, files::CreateKind::EmptyFile);
    pump.create(service, "Document 文.txt");
    TEST_CHECK(
        service.snapshot().status == files::CreateStatus::Succeeded
        && service.snapshot().kind == files::CreateKind::EmptyFile
        && service.snapshot().destination == location(fixture.root / "Document 文.txt")
        && fs::is_regular_file(fixture.root / "Document 文.txt")
        && fs::file_size(fixture.root / "Document 文.txt") == 0
    );
    std::ofstream(fixture.root / "protected.txt") << "do not truncate";
    fs::create_directory(fixture.root / "directory");
    std::ofstream(fixture.root / "directory/marker") << "keep";
    fs::create_symlink("missing-target", fixture.root / "broken-link");
    fs::create_symlink("protected.txt", fixture.root / "link");
    TEST_CHECK(::mkfifo((fixture.root / "pipe").c_str(), 0600) == 0);
    pump.prepare(service, fixture.root, files::CreateKind::EmptyFile);
    for (const auto* name : {"protected.txt", "directory", "broken-link", "link", "pipe"}) {
      pump.create(service, name);
      TEST_CHECK(service.snapshot().status == files::CreateStatus::Error && service.canCreate());
    }
    std::ifstream content(fixture.root / "protected.txt");
    const std::string bytes{std::istreambuf_iterator<char>(content), {}};
    TEST_CHECK(
        bytes == "do not truncate"
        && fs::exists(fixture.root / "directory/marker")
        && fs::is_symlink(fixture.root / "broken-link")
        && fs::is_symlink(fixture.root / "link")
        && !fs::exists(fixture.root / "missing-target")
        && fs::is_fifo(fixture.root / "pipe")
    );
    TEST_CHECK(!service.create("../escaped"));
    pump.create(service, "retry.txt");
    TEST_CHECK(
        service.snapshot().status == files::CreateStatus::Succeeded && fs::file_size(fixture.root / "retry.txt") == 0
    );
    files::CreateService other;
    pump.prepare(service, fixture.root, files::CreateKind::EmptyFile);
    pump.prepare(other, fixture.root, files::CreateKind::EmptyFile);
    TEST_CHECK(service.create("racing.txt") && other.create("racing.txt"));
    pump.until([&] { return !service.busy() && !other.busy(); });
    TEST_CHECK(
        (service.snapshot().status == files::CreateStatus::Succeeded)
            != (other.snapshot().status == files::CreateStatus::Succeeded)
        && fs::file_size(fixture.root / "racing.txt") == 0
    );
    pump.prepare(service, fixture.root, files::CreateKind::EmptyFile);
    TEST_CHECK(service.create("cancelled.txt"));
    service.cancel();
    pump.until([&] { return !service.busy(); });
    const bool committed = service.snapshot().status == files::CreateStatus::Succeeded;
    TEST_CHECK(
        (committed || service.snapshot().status == files::CreateStatus::Cancelled)
        && fs::exists(fixture.root / "cancelled.txt") == committed
    );
    const auto raw = std::string("raw-") + static_cast<char>(0xff);
    fs::create_directory(fixture.root / raw);
    pump.prepare(service, fixture.root / raw, files::CreateKind::EmptyFile);
    pump.create(service, "child.txt");
    TEST_CHECK(fs::is_regular_file(fixture.root / raw / "child.txt"));
    struct MaskGuard {
      mode_t previous = ::umask(0027);
      ~MaskGuard() { ::umask(previous); }
    } guard;
    pump.prepare(service, fixture.root, files::CreateKind::EmptyFile);
    pump.create(service, "mode.txt");
    struct stat info{};
    TEST_CHECK(::stat((fixture.root / "mode.txt").c_str(), &info) == 0 && (info.st_mode & 0777) == 0640);
    if (::geteuid() != 0) {
      pump.prepare(service, fixture.root, files::CreateKind::EmptyFile);
      fs::permissions(fixture.root, fs::perms::owner_read | fs::perms::owner_exec);
      pump.create(service, "denied.txt");
      fs::permissions(fixture.root, fs::perms::owner_all);
      TEST_CHECK(service.snapshot().status == files::CreateStatus::Error && !fs::exists(fixture.root / "denied.txt"));
    }
  }
  void monitoredCreation(Pump& pump) {
    Fixture fixture;
    files::DirectoryModel model;
    files::DirectorySession session;
    session.setChangedCallback([&] { model.applySnapshot(session.snapshot()); });
    session.open(location(fixture.root));
    pump.until([&] { return model.snapshot().status == files::DirectoryStatus::Ready; });
    files::CreateService service;
    pump.prepare(service, fixture.root);
    pump.create(service, "created");
    pump.until([&] { return model.rows().size() == 1; });
    const auto id = model.rows().front();
    TEST_CHECK(
        model.entry(id)->location == service.snapshot().destination
        && model.entry(id)->type == G_FILE_TYPE_DIRECTORY
        && model.select(id)
    );
    pump.prepare(service, fixture.root, files::CreateKind::EmptyFile);
    pump.create(service, "created.txt");
    pump.until([&] { return model.rows().size() == 2; });
    const auto file = std::ranges::find_if(model.rows(), [&](auto id) {
      return model.entry(id)->location == service.snapshot().destination;
    });
    TEST_CHECK(
        file != model.rows().end()
        && model.entry(*file)->type == G_FILE_TYPE_REGULAR
        && model.entry(*file)->size == 0
        && model.select(*file)
    );
  }
} // namespace
int main() {
  try {
    Pump pump;
    createsFolders(pump);
    createsEmptyFiles(pump);
    collisionsAndRetry(pump);
    staleDestinations(pump, files::CreateKind::Folder);
    staleDestinations(pump, files::CreateKind::EmptyFile);
    validationCancellationAndPermissions(pump);
    monitoredCreation(pump);
    std::println("create-service tests passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "create-service: {}", error.what());
    return 1;
  }
}
