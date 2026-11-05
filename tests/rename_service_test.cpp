#include "app/glib_poll_source.h"
#include "browser/directory_model.h"
#include "fs/filename.h"
#include "operations/rename_service.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  void require(bool value, const char* message) {
    if (!value) {
      throw std::runtime_error(message);
    }
  }
  void write(const fs::path& path, const std::string& text) { std::ofstream(path) << text; }
  std::string read(const fs::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), {}};
  }
  files::Location location(const fs::path& path) { return files::Location::fromPath(path.native()); }
  struct Fixture {
    fs::path root;
    Fixture() {
      std::string path = "/tmp/noctalia-rename-XXXXXX";
      require(::mkdtemp(path.data()) != nullptr, "fixture failed");
      root = path;
    }
    ~Fixture() {
      std::error_code error;
      fs::remove_all(root, error);
    }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!done()) {
        require(std::chrono::steady_clock::now() < deadline, "rename timed out");
        std::vector<pollfd> fds;
        const auto first = source.addPollFds(fds);
        require(::poll(fds.data(), fds.size(), 1) >= 0, "poll failed");
        source.dispatch(fds, first);
      }
    }
    void prepare(files::RenameService& service, const fs::path& path) {
      require(service.prepare(location(path)), "preparation refused");
      until([&] { return !service.busy(); });
      require(service.canRename(), "preparation failed");
    }
    void rename(files::RenameService& service, const std::string& name) {
      require(service.rename(name), "rename refused");
      until([&] { return !service.busy(); });
    }

  private:
    GlibPollSource source;
  };
  void successfulRenames(Pump& pump) {
    Fixture fixture;
    files::RenameService service;
    write(fixture.root / "source.txt", "content");
    pump.prepare(service, fixture.root / "source.txt");
    pump.rename(service, "résumé 文.txt");
    require(
        service.snapshot().status == files::RenameStatus::SUCCEEDED
            && read(fixture.root / "résumé 文.txt") == "content"
            && !fs::exists(fixture.root / "source.txt"),
        "unicode rename failed"
    );
    require(
        service.snapshot().destination == location(fixture.root / "résumé 文.txt"), "destination URI lost identity"
    );
    pump.prepare(service, fixture.root / "résumé 文.txt");
    pump.rename(service, "résumé 文.txt");
    require(service.snapshot().status == files::RenameStatus::SUCCEEDED, "same-name rename failed");
    pump.prepare(service, fixture.root / "résumé 文.txt");
    pump.rename(service, "RÉSUMÉ 文.txt");
    require(read(fixture.root / "RÉSUMÉ 文.txt") == "content", "case-only rename failed");
    fs::create_directory(fixture.root / "folder");
    write(fixture.root / "folder/child", "nested");
    pump.prepare(service, fixture.root / "folder");
    pump.rename(service, "new folder");
    require(read(fixture.root / "new folder/child") == "nested", "folder contents changed");
    fs::create_symlink("missing", fixture.root / "broken");
    pump.prepare(service, fixture.root / "broken");
    pump.rename(service, "new link");
    require(fs::read_symlink(fixture.root / "new link") == "missing", "symlink was followed");
    const std::string raw = std::string("raw-") + static_cast<char>(0xff);
    write(fixture.root / raw, "raw data");
    pump.prepare(service, fixture.root / raw);
    pump.rename(service, "readable");
    require(read(fixture.root / "readable") == "raw data", "raw source name lost");
  }
  void collisionsAndReplacement(Pump& pump) {
    Fixture fixture;
    files::RenameService service;
    write(fixture.root / "source", "source");
    write(fixture.root / "existing", "existing");
    pump.prepare(service, fixture.root / "source");
    pump.rename(service, "existing");
    require(
        service.snapshot().status == files::RenameStatus::ERROR
            && service.canRename()
            && read(fixture.root / "existing") == "existing"
            && read(fixture.root / "source") == "source",
        "collision overwrote data"
    );
    fs::create_directory(fixture.root / "directory");
    pump.rename(service, "directory");
    require(
        service.snapshot().status == files::RenameStatus::ERROR && fs::is_directory(fixture.root / "directory"),
        "directory collision changed target"
    );
    fs::create_symlink("absent", fixture.root / "symlink");
    pump.rename(service, "symlink");
    require(
        service.snapshot().status == files::RenameStatus::ERROR && fs::is_symlink(fixture.root / "symlink"),
        "broken-link collision was overwritten"
    );
    fs::create_hard_link(fixture.root / "source", fixture.root / "hard-link");
    pump.rename(service, "hard-link");
    require(
        service.snapshot().status == files::RenameStatus::ERROR
            && fs::equivalent(fixture.root / "source", fixture.root / "hard-link"),
        "hard-link collision was accepted"
    );
    pump.rename(service, "available");
    require(service.snapshot().status == files::RenameStatus::SUCCEEDED, "retry failed");
    write(fixture.root / "source", "original");
    pump.prepare(service, fixture.root / "source");
    fs::rename(fixture.root / "source", fixture.root / "original");
    write(fixture.root / "source", "replacement");
    pump.rename(service, "renamed");
    require(
        service.snapshot().status == files::RenameStatus::ERROR
            && read(fixture.root / "source") == "replacement"
            && read(fixture.root / "original") == "original"
            && !fs::exists(fixture.root / "renamed"),
        "replaced source was renamed"
    );
    fs::create_directory(fixture.root / "parent");
    write(fixture.root / "parent/item", "original");
    pump.prepare(service, fixture.root / "parent/item");
    fs::rename(fixture.root / "parent", fixture.root / "old-parent");
    fs::create_directory(fixture.root / "parent");
    write(fixture.root / "parent/item", "replacement");
    pump.rename(service, "renamed");
    require(
        service.snapshot().status == files::RenameStatus::ERROR
            && read(fixture.root / "old-parent/item") == "original"
            && read(fixture.root / "parent/item") == "replacement",
        "replaced parent was retargeted"
    );
  }
  void validationAndCancellation(Pump& pump) {
    Fixture fixture;
    files::RenameService service;
    write(fixture.root / "source", "data");
    pump.prepare(service, fixture.root / "source");
    for (const auto& name : std::vector<std::string>{
             "", ".", "..", "child/name", std::string("a\0b", 3), std::string(1, static_cast<char>(0xff))
         }) {
      require(!service.rename(name) && fs::exists(fixture.root / "source"), "invalid filename accepted");
    }
    pump.rename(service, std::string(10000, 'a'));
    require(
        service.snapshot().status == files::RenameStatus::ERROR && fs::exists(fixture.root / "source"),
        "oversized name changed source"
    );
    service.cancel();
    require(!service.canRename(), "cancel kept prepared target");
    write(fixture.root / "editable", "before");
    pump.prepare(service, fixture.root / "editable");
    write(fixture.root / "editable", "updated while editing");
    pump.rename(service, "edited");
    require(
        service.snapshot().status == files::RenameStatus::SUCCEEDED
            && read(fixture.root / "edited") == "updated while editing",
        "content edits to the same source were lost"
    );
    pump.prepare(service, fixture.root / "source");
    require(service.rename("cancel-result"), "rename was refused");
    service.cancel();
    pump.until([&] { return !service.busy(); });
    const bool committed = service.snapshot().status == files::RenameStatus::SUCCEEDED;
    require(
        (committed || service.snapshot().status == files::RenameStatus::CANCELLED)
            && fs::exists(fixture.root / "cancel-result") == committed
            && fs::exists(fixture.root / "source") != committed,
        "cancellation reported an incorrect committed outcome"
    );
    if (committed) {
      fs::rename(fixture.root / "cancel-result", fixture.root / "source");
    }
    require(service.prepare(location(fixture.root / "source")), "preparation refused");
    require(!service.prepare(location(fixture.root / "source")) && !service.rename("other"), "busy operation accepted");
    service.cancel();
    pump.until([&] { return !service.busy(); });
    require(
        service.snapshot().status == files::RenameStatus::CANCELLED && fs::exists(fixture.root / "source"),
        "preparation cancellation failed"
    );
    require(
        service.prepare(files::Location::fromUri("sftp://example.invalid/file")),
        "remote request refused before dispatch"
    );
    pump.until([&] { return !service.busy(); });
    require(
        service.snapshot().status == files::RenameStatus::ERROR && !service.canRename(), "remote rename not rejected"
    );
    require(service.prepare(location(fixture.root / "missing")), "missing request refused");
    pump.until([&] { return !service.busy(); });
    require(service.snapshot().status == files::RenameStatus::ERROR, "missing source accepted");
    require(
        files::filenameStemLength("archive.tar.gz", false) == 11
            && files::filenameStemLength(".bashrc", false) == 7
            && files::filenameStemLength("folder.name", true) == 11
            && files::filenameStemLength("trailing.", false) == 9,
        "basename selection boundaries changed"
    );
    if (::geteuid() != 0) {
      pump.prepare(service, fixture.root / "source");
      fs::permissions(fixture.root, fs::perms::owner_read | fs::perms::owner_exec);
      pump.rename(service, "denied");
      fs::permissions(fixture.root, fs::perms::owner_all);
      require(
          service.snapshot().status == files::RenameStatus::ERROR && fs::exists(fixture.root / "source"),
          "permission failure lost source"
      );
    }
  }
  void selectionSurvives(Pump& pump) {
    Fixture fixture;
    write(fixture.root / "before.txt", "content");
    files::DirectoryModel model;
    files::DirectorySession session;
    session.setChangedCallback([&] { model.applySnapshot(session.snapshot()); });
    session.open(location(fixture.root));
    pump.until([&] { return model.snapshot().status == files::DirectoryStatus::READY && model.rows().size() == 1; });
    const auto id = model.rows().front();
    require(model.select(id), "selection failed");
    files::RenameService service;
    pump.prepare(service, fixture.root / "before.txt");
    pump.rename(service, "after.txt");
    pump.until([&] {
      const auto* entry = model.entry(id);
      return entry != nullptr && entry->name == "after.txt";
    });
    require(
        model.selection().selected.contains(id) && model.selection().focused == id,
        "monitored rename lost selection identity"
    );
  }
} // namespace
int main() {
  try {
    Pump pump;
    successfulRenames(pump);
    collisionsAndReplacement(pump);
    validationAndCancellation(pump);
    selectionSurvives(pump);
    std::println("rename-service tests passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "rename-service: {}", error.what());
    return 1;
  }
}
