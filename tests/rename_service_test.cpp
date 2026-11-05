#include "app/glib_poll_source.h"
#include "browser/directory_model.h"
#include "fs/filename.h"
#include "operations/rename_service.h"
#include "test_check.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
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
      TEST_CHECK(::mkdtemp(path.data()) != nullptr);
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
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto first = source.addPollFds(fds);
        TEST_CHECK(::poll(fds.data(), fds.size(), 1) >= 0);
        source.dispatch(fds, first);
      }
    }
    void prepare(files::RenameService& service, const fs::path& path) {
      TEST_CHECK(service.prepare(location(path)));
      until([&] { return !service.busy(); });
      TEST_CHECK(service.canRename());
    }
    void rename(files::RenameService& service, const std::string& name) {
      TEST_CHECK(service.rename(name));
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
    TEST_CHECK(
        service.snapshot().status == files::RenameStatus::Succeeded
        && read(fixture.root / "résumé 文.txt") == "content"
        && !fs::exists(fixture.root / "source.txt")
    );
    TEST_CHECK(service.snapshot().destination == location(fixture.root / "résumé 文.txt"));
    pump.prepare(service, fixture.root / "résumé 文.txt");
    pump.rename(service, "résumé 文.txt");
    TEST_CHECK(service.snapshot().status == files::RenameStatus::Succeeded);
    pump.prepare(service, fixture.root / "résumé 文.txt");
    pump.rename(service, "RÉSUMÉ 文.txt");
    TEST_CHECK(read(fixture.root / "RÉSUMÉ 文.txt") == "content");
    fs::create_directory(fixture.root / "folder");
    write(fixture.root / "folder/child", "nested");
    pump.prepare(service, fixture.root / "folder");
    pump.rename(service, "new folder");
    TEST_CHECK(read(fixture.root / "new folder/child") == "nested");
    fs::create_symlink("missing", fixture.root / "broken");
    pump.prepare(service, fixture.root / "broken");
    pump.rename(service, "new link");
    TEST_CHECK(fs::read_symlink(fixture.root / "new link") == "missing");
    const std::string raw = std::string("raw-") + static_cast<char>(0xff);
    write(fixture.root / raw, "raw data");
    pump.prepare(service, fixture.root / raw);
    pump.rename(service, "readable");
    TEST_CHECK(read(fixture.root / "readable") == "raw data");
  }
  void collisionsAndReplacement(Pump& pump) {
    Fixture fixture;
    files::RenameService service;
    write(fixture.root / "source", "source");
    write(fixture.root / "existing", "existing");
    pump.prepare(service, fixture.root / "source");
    pump.rename(service, "existing");
    TEST_CHECK(
        service.snapshot().status == files::RenameStatus::Error
        && service.canRename()
        && read(fixture.root / "existing") == "existing"
        && read(fixture.root / "source") == "source"
    );
    fs::create_directory(fixture.root / "directory");
    pump.rename(service, "directory");
    TEST_CHECK(service.snapshot().status == files::RenameStatus::Error && fs::is_directory(fixture.root / "directory"));
    fs::create_symlink("absent", fixture.root / "symlink");
    pump.rename(service, "symlink");
    TEST_CHECK(service.snapshot().status == files::RenameStatus::Error && fs::is_symlink(fixture.root / "symlink"));
    fs::create_hard_link(fixture.root / "source", fixture.root / "hard-link");
    pump.rename(service, "hard-link");
    TEST_CHECK(
        service.snapshot().status == files::RenameStatus::Error
        && fs::equivalent(fixture.root / "source", fixture.root / "hard-link")
    );
    pump.rename(service, "available");
    TEST_CHECK(service.snapshot().status == files::RenameStatus::Succeeded);
    write(fixture.root / "source", "original");
    pump.prepare(service, fixture.root / "source");
    fs::rename(fixture.root / "source", fixture.root / "original");
    write(fixture.root / "source", "replacement");
    pump.rename(service, "renamed");
    TEST_CHECK(
        service.snapshot().status == files::RenameStatus::Error
        && read(fixture.root / "source") == "replacement"
        && read(fixture.root / "original") == "original"
        && !fs::exists(fixture.root / "renamed")
    );
    fs::create_directory(fixture.root / "parent");
    write(fixture.root / "parent/item", "original");
    pump.prepare(service, fixture.root / "parent/item");
    fs::rename(fixture.root / "parent", fixture.root / "old-parent");
    fs::create_directory(fixture.root / "parent");
    write(fixture.root / "parent/item", "replacement");
    pump.rename(service, "renamed");
    TEST_CHECK(
        service.snapshot().status == files::RenameStatus::Error
        && read(fixture.root / "old-parent/item") == "original"
        && read(fixture.root / "parent/item") == "replacement"
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
      TEST_CHECK(!service.rename(name) && fs::exists(fixture.root / "source"));
    }
    pump.rename(service, std::string(10000, 'a'));
    TEST_CHECK(service.snapshot().status == files::RenameStatus::Error && fs::exists(fixture.root / "source"));
    service.cancel();
    TEST_CHECK(!service.canRename());
    write(fixture.root / "editable", "before");
    pump.prepare(service, fixture.root / "editable");
    write(fixture.root / "editable", "updated while editing");
    pump.rename(service, "edited");
    TEST_CHECK(
        service.snapshot().status == files::RenameStatus::Succeeded
        && read(fixture.root / "edited") == "updated while editing"
    );
    pump.prepare(service, fixture.root / "source");
    TEST_CHECK(service.rename("cancel-result"));
    service.cancel();
    pump.until([&] { return !service.busy(); });
    const bool committed = service.snapshot().status == files::RenameStatus::Succeeded;
    TEST_CHECK(
        (committed || service.snapshot().status == files::RenameStatus::Cancelled)
        && fs::exists(fixture.root / "cancel-result") == committed
        && fs::exists(fixture.root / "source") != committed
    );
    if (committed) {
      fs::rename(fixture.root / "cancel-result", fixture.root / "source");
    }
    TEST_CHECK(service.prepare(location(fixture.root / "source")));
    TEST_CHECK(!service.prepare(location(fixture.root / "source")) && !service.rename("other"));
    service.cancel();
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == files::RenameStatus::Cancelled && fs::exists(fixture.root / "source"));
    TEST_CHECK(service.prepare(files::Location::fromUri("sftp://example.invalid/file")));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == files::RenameStatus::Error && !service.canRename());
    TEST_CHECK(service.prepare(location(fixture.root / "missing")));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(service.snapshot().status == files::RenameStatus::Error);
    TEST_CHECK(
        files::filenameStemLength("archive.tar.gz", false) == 11
        && files::filenameStemLength(".bashrc", false) == 7
        && files::filenameStemLength("folder.name", true) == 11
        && files::filenameStemLength("trailing.", false) == 9
    );
    if (::geteuid() != 0) {
      pump.prepare(service, fixture.root / "source");
      fs::permissions(fixture.root, fs::perms::owner_read | fs::perms::owner_exec);
      pump.rename(service, "denied");
      fs::permissions(fixture.root, fs::perms::owner_all);
      TEST_CHECK(service.snapshot().status == files::RenameStatus::Error && fs::exists(fixture.root / "source"));
    }
  }
  void selectionSurvives(Pump& pump) {
    Fixture fixture;
    write(fixture.root / "before.txt", "content");
    files::DirectoryModel model;
    files::DirectorySession session;
    session.setChangedCallback([&] { model.applySnapshot(session.snapshot()); });
    session.open(location(fixture.root));
    pump.until([&] { return model.snapshot().status == files::DirectoryStatus::Ready && model.rows().size() == 1; });
    const auto id = model.rows().front();
    TEST_CHECK(model.select(id));
    files::RenameService service;
    pump.prepare(service, fixture.root / "before.txt");
    pump.rename(service, "after.txt");
    pump.until([&] {
      const auto* entry = model.entry(id);
      return entry != nullptr && entry->name == "after.txt";
    });
    TEST_CHECK(model.selection().selected.contains(id) && model.selection().focused == id);
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
