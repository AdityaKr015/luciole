#include "app/glib_poll_source.h"
#include "browser/directory_model.h"
#include "operations/create_folder_service.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  files::Location location(const fs::path& path) { return files::Location::fromPath(path.native()); }
  struct Fixture {
    fs::path root;
    Fixture() {
      std::string path = "/tmp/noctalia-create-folder-XXXXXX";
      require(::mkdtemp(path.data()) != nullptr, "fixture failed");
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
        require(std::chrono::steady_clock::now() < deadline, "creation timed out");
        std::vector<pollfd> fds;
        const auto first = source.addPollFds(fds);
        require(::poll(fds.data(), fds.size(), 1) >= 0, "poll failed");
        source.dispatch(fds, first);
      }
    }
    void prepare(files::CreateFolderService& service, const fs::path& path) {
      require(service.prepare(location(path)), "preparation refused");
      until([&] { return !service.busy(); });
      require(service.canCreate(), "preparation failed");
    }
    void create(files::CreateFolderService& service, const std::string& name) {
      require(service.create(name), "creation refused");
      until([&] { return !service.busy(); });
    }

  private:
    GlibPollSource source;
  };
  void createsFolders(Pump& pump) {
    Fixture fixture;
    files::CreateFolderService service;
    pump.prepare(service, fixture.root);
    pump.create(service, "Documents 文");
    require(
        service.snapshot().status == files::CreateFolderStatus::SUCCEEDED
            && fs::is_empty(fixture.root / "Documents 文"),
        "Unicode folder creation failed"
    );
    require(service.snapshot().destination == location(fixture.root / "Documents 文"), "destination URI changed");
    pump.prepare(service, fixture.root);
    pump.create(service, ".hidden");
    require(fs::is_directory(fixture.root / ".hidden"), "hidden folder creation failed");
    const auto raw = std::string("raw-") + static_cast<char>(0xff);
    fs::create_directory(fixture.root / raw);
    pump.prepare(service, fixture.root / raw);
    pump.create(service, "child");
    require(fs::is_directory(fixture.root / raw / "child"), "raw parent name lost");
    fs::create_directory_symlink(fixture.root, fixture.root / "alias");
    pump.prepare(service, fixture.root / "alias");
    pump.create(service, "through link");
    require(fs::is_directory(fixture.root / "through link"), "symlink destination failed");
  }
  void collisionsAndRetry(Pump& pump) {
    Fixture fixture;
    fs::create_directory(fixture.root / "folder");
    std::ofstream(fixture.root / "folder/marker") << "preserve";
    std::ofstream(fixture.root / "file") << "data";
    fs::create_symlink("missing", fixture.root / "link");
    files::CreateFolderService service;
    pump.prepare(service, fixture.root);
    for (const auto* name : {"folder", "file", "link"}) {
      pump.create(service, name);
      require(
          service.snapshot().status == files::CreateFolderStatus::ERROR && service.canCreate(),
          "collision was accepted or retry unavailable"
      );
    }
    require(
        fs::exists(fixture.root / "folder/marker")
            && fs::is_regular_file(fixture.root / "file")
            && fs::read_symlink(fixture.root / "link") == "missing",
        "collision modified an existing entry"
    );
    pump.create(service, "available");
    require(service.snapshot().status == files::CreateFolderStatus::SUCCEEDED, "collision retry failed");
    files::CreateFolderService other;
    pump.prepare(service, fixture.root);
    pump.prepare(other, fixture.root);
    require(service.create("racing") && other.create("racing"), "racing requests refused");
    pump.until([&] { return !service.busy() && !other.busy(); });
    const bool first = service.snapshot().status == files::CreateFolderStatus::SUCCEEDED;
    const bool second = other.snapshot().status == files::CreateFolderStatus::SUCCEEDED;
    require(first != second && fs::is_directory(fixture.root / "racing"), "creation race did not produce one winner");
  }
  void staleDestinations(Pump& pump) {
    Fixture fixture;
    fs::create_directory(fixture.root / "parent");
    files::CreateFolderService service;
    pump.prepare(service, fixture.root / "parent");
    fs::rename(fixture.root / "parent", fixture.root / "original");
    fs::create_directory(fixture.root / "parent");
    pump.create(service, "blocked");
    require(
        service.snapshot().status == files::CreateFolderStatus::ERROR
            && !fs::exists(fixture.root / "parent/blocked")
            && !fs::exists(fixture.root / "original/blocked"),
        "replaced destination was retargeted"
    );
    fs::create_directory_symlink(fixture.root / "parent", fixture.root / "alias");
    pump.prepare(service, fixture.root / "alias");
    fs::remove(fixture.root / "alias");
    fs::create_directory_symlink(fixture.root / "original", fixture.root / "alias");
    pump.create(service, "blocked");
    require(
        service.snapshot().status == files::CreateFolderStatus::ERROR && fs::is_empty(fixture.root / "original"),
        "changed parent symlink was retargeted"
    );
  }
  void validationCancellationAndPermissions(Pump& pump) {
    Fixture fixture;
    files::CreateFolderService service;
    pump.prepare(service, fixture.root);
    for (const auto& name : std::vector<std::string>{
             "", ".", "..", "nested/name", std::string("a\0b", 3), std::string(1, static_cast<char>(0xff))
         })
      require(!service.create(name) && fs::is_empty(fixture.root), "invalid name created an item");
    pump.create(service, std::string(10000, 'a'));
    require(
        service.snapshot().status == files::CreateFolderStatus::ERROR && fs::is_empty(fixture.root),
        "oversized name was accepted"
    );
    service.cancel();
    require(!service.canCreate() && fs::is_empty(fixture.root), "cancel created a folder or kept the prepared target");
    require(service.prepare(location(fixture.root)), "preparation refused");
    require(!service.prepare(location(fixture.root)) && !service.create("busy"), "busy operation accepted");
    service.cancel();
    pump.until([&] { return !service.busy(); });
    require(
        service.snapshot().status == files::CreateFolderStatus::CANCELLED && fs::is_empty(fixture.root),
        "cancelled preparation created an item"
    );
    pump.prepare(service, fixture.root);
    require(service.create("maybe-created"), "creation refused");
    service.cancel();
    pump.until([&] { return !service.busy(); });
    const bool committed = service.snapshot().status == files::CreateFolderStatus::SUCCEEDED;
    require(
        (committed || service.snapshot().status == files::CreateFolderStatus::CANCELLED)
            && fs::exists(fixture.root / "maybe-created") == committed,
        "cancellation misreported a completed creation"
    );
    for (const auto& parent :
         {location(fixture.root / "missing"), files::Location::fromUri("sftp://example.invalid/folder")}) {
      require(service.prepare(parent), "invalid parent request refused");
      pump.until([&] { return !service.busy(); });
      require(
          service.snapshot().status == files::CreateFolderStatus::ERROR && !service.canCreate(),
          "invalid destination was accepted"
      );
    }
    if (::geteuid() != 0) {
      pump.prepare(service, fixture.root);
      fs::permissions(fixture.root, fs::perms::owner_read | fs::perms::owner_exec);
      pump.create(service, "denied");
      fs::permissions(fixture.root, fs::perms::owner_all);
      require(
          service.snapshot().status == files::CreateFolderStatus::ERROR && !fs::exists(fixture.root / "denied"),
          "permission failure created a folder"
      );
    }
    struct MaskGuard {
      mode_t previous = ::umask(0027);
      ~MaskGuard() { ::umask(previous); }
    } guard;
    pump.prepare(service, fixture.root);
    pump.create(service, "permissions");
    struct stat info{};
    require(
        ::stat((fixture.root / "permissions").c_str(), &info) == 0 && (info.st_mode & 0777) == 0750,
        "creation did not honor umask"
    );
  }
  void monitoredCreation(Pump& pump) {
    Fixture fixture;
    files::DirectoryModel model;
    files::DirectorySession session;
    session.setChangedCallback([&] { model.applySnapshot(session.snapshot()); });
    session.open(location(fixture.root));
    pump.until([&] { return model.snapshot().status == files::DirectoryStatus::READY; });
    files::CreateFolderService service;
    pump.prepare(service, fixture.root);
    pump.create(service, "created");
    pump.until([&] { return model.rows().size() == 1; });
    const auto id = model.rows().front();
    require(
        model.entry(id)->location == service.snapshot().destination
            && model.entry(id)->type == G_FILE_TYPE_DIRECTORY
            && model.select(id),
        "created folder was not actionable in the live model"
    );
  }
} // namespace
int main() {
  try {
    Pump pump;
    createsFolders(pump);
    collisionsAndRetry(pump);
    staleDestinations(pump);
    validationCancellationAndPermissions(pump);
    monitoredCreation(pump);
    std::println("create-folder-service tests passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "create-folder-service: {}", error.what());
    return 1;
  }
}
