#include "app/glib_poll_source.h"
#include "browser/directory_model.h"
#include "operations/trash_service.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  void require(bool value, const char* message) {
    if (!value) {
      throw std::runtime_error(message);
    }
  }
  files::Location location(const fs::path& path) { return files::Location::fromPath(path.native()); }
  void write(const fs::path& path, const std::string& value) { std::ofstream(path) << value; }
  std::string read(const fs::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  struct Fixture {
    fs::path root, home, data;
    Fixture() {
      std::string path = "/tmp/noctalia-trash-XXXXXX";
      require(::mkdtemp(path.data()) != nullptr, "fixture failed");
      root = path;
      home = root / "home";
      data = home / ".local/share";
      fs::create_directories(data);
      require(::setenv("HOME", home.c_str(), 1) == 0, "private home failed");
      require(::setenv("XDG_DATA_HOME", data.c_str(), 1) == 0, "private trash failed");
    }
    ~Fixture() {
      std::error_code error;
      fs::remove_all(root, error);
    }
    fs::path folder(const std::string& name) const {
      const auto path = home / name;
      fs::create_directories(path);
      return path;
    }
    std::vector<fs::path> archived(const fs::path& original) const {
      std::vector<fs::path> paths;
      const auto info = data / "Trash/info";
      if (!fs::exists(info)) {
        return paths;
      }
      for (const auto& entry : fs::directory_iterator(info)) {
        std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)> key(g_key_file_new(), g_key_file_unref);
        require(
            g_key_file_load_from_file(key.get(), entry.path().c_str(), G_KEY_FILE_NONE, nullptr), "invalid trashinfo"
        );
        const files::GString encoded(g_key_file_get_string(key.get(), "Trash Info", "Path", nullptr));
        require(encoded != nullptr, "missing original path");
        const files::GString decoded(g_uri_unescape_string(encoded.get(), nullptr));
        if (decoded && original.native() == decoded.get()) {
          const files::GString date(g_key_file_get_string(key.get(), "Trash Info", "DeletionDate", nullptr));
          require(date && std::string(date.get()).size() == 19, "missing deletion date");
          paths.push_back(data / "Trash/files" / entry.path().stem());
        }
      }
      return paths;
    }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!done()) {
        require(std::chrono::steady_clock::now() < deadline, "trash timed out");
        std::vector<pollfd> fds;
        const auto first = source.addPollFds(fds);
        require(::poll(fds.data(), fds.size(), 1) >= 0, "poll failed");
        source.dispatch(fds, first);
      }
    }
    void prepare(files::TrashService& service, const fs::path& parent, const std::vector<fs::path>& paths) {
      std::vector<files::Location> sources;
      sources.reserve(paths.size());
      for (const auto& path : paths) {
        sources.push_back(location(path));
      }
      require(service.prepare(location(parent), std::move(sources)), "preparation refused");
      until([&] { return !service.busy(); });
      require(service.canTrash(), "preparation failed");
    }
    void trash(files::TrashService& service) {
      require(service.trash(), "trash refused");
      until([&] { return !service.busy(); });
      require(service.snapshot().status == files::TrashStatus::FINISHED, "trash did not report outcomes");
    }

  private:
    GlibPollSource source;
  };
  void preservesRecoverableContent(const Fixture& fixture, Pump& pump) {
    const auto parent = fixture.folder("content");
    const auto file = parent / "résumé 文 #%.txt";
    write(file, "recoverable");
    fs::create_directory(parent / "folder");
    write(parent / "folder/child", "nested");
    const auto outside = fixture.folder("outside");
    write(outside / "keep", "untouched");
    fs::create_directory_symlink(outside, parent / "folder/external");
    fs::create_symlink(file, parent / "link");
    fs::create_symlink("absent", parent / "broken");
    require(::mkfifo((parent / "fifo").c_str(), 0600) == 0, "FIFO fixture failed");
    files::TrashService service;
    pump.prepare(service, parent, {file, parent / "folder", parent / "link", parent / "broken", parent / "fifo"});
    require(fs::exists(file), "prepare mutated source");
    pump.trash(service);
    require(service.snapshot().results.size() == 5 && service.completed() == 5, "result count changed");
    require(
        std::ranges::all_of(
            service.snapshot().results, [](const auto& item) { return item.outcome == files::TrashOutcome::TRASHED; }
        ),
        "native trash failed"
    );
    require(fs::is_empty(parent), "source entries survived successful trash");
    const auto files = fixture.archived(file);
    const auto folders = fixture.archived(parent / "folder");
    require(files.size() == 1 && read(files.front()) == "recoverable", "trashed data or original path lost");
    require(folders.size() == 1 && read(folders.front() / "child") == "nested", "folder contents lost");
    require(read(outside / "keep") == "untouched", "nested symlink target modified");
    const auto links = fixture.archived(parent / "link");
    const auto broken = fixture.archived(parent / "broken");
    require(links.size() == 1 && fs::read_symlink(links.front()) == file, "file symlink followed");
    require(broken.size() == 1 && fs::read_symlink(broken.front()) == "absent", "broken symlink lost");
    const auto fifos = fixture.archived(parent / "fifo");
    require(fifos.size() == 1 && fs::is_fifo(fifos.front()), "FIFO was streamed or lost");
    write(file, "second version");
    pump.prepare(service, parent, {file});
    pump.trash(service);
    const auto versions = fixture.archived(file);
    require(versions.size() == 2, "trash basename collision overwrote existing item");
    require(
        std::ranges::any_of(versions, [](const auto& path) { return read(path) == "recoverable"; })
            && std::ranges::any_of(versions, [](const auto& path) { return read(path) == "second version"; }),
        "trash collision damaged contents"
    );
  }
  void rawNames(const Fixture& fixture, Pump& pump) {
    const auto raw = std::string("raw-") + static_cast<char>(0xff);
    const auto parent = fixture.folder(raw);
    const auto file = parent / raw;
    write(file, "raw");
    files::TrashService service;
    pump.prepare(service, parent, {file});
    pump.trash(service);
    const auto archived = fixture.archived(file);
    require(archived.size() == 1 && read(archived.front()) == "raw", "raw filename or parent lost");
  }
  void staleTargetsAndPartialResults(const Fixture& fixture, Pump& pump) {
    const auto parent = fixture.folder("partial");
    for (const auto* name : {"replaced", "good", "missing"}) {
      write(parent / name, name);
    }
    files::TrashService service;
    pump.prepare(service, parent, {parent / "replaced", parent / "good", parent / "missing"});
    fs::rename(parent / "replaced", parent / "original");
    write(parent / "replaced", "replacement");
    fs::remove(parent / "missing");
    pump.trash(service);
    const auto& results = service.snapshot().results;
    require(
        results[0].outcome == files::TrashOutcome::FAILED
            && results[1].outcome == files::TrashOutcome::TRASHED
            && results[2].outcome == files::TrashOutcome::FAILED,
        "partial outcomes were hidden or retargeted"
    );
    require(
        read(parent / "replaced") == "replacement" && read(parent / "original") == "replaced", "replaced source trashed"
    );
    const auto stale = fixture.folder("stale");
    write(stale / "item", "original");
    pump.prepare(service, stale, {stale / "item"});
    fs::rename(stale, fixture.home / "old-stale");
    fs::create_directory(stale);
    write(stale / "item", "replacement");
    pump.trash(service);
    require(
        service.snapshot().results.front().outcome == files::TrashOutcome::FAILED
            && read(stale / "item") == "replacement"
            && read(fixture.home / "old-stale/item") == "original",
        "replaced parent was followed"
    );
    const auto alias = fixture.home / "alias";
    fs::create_directory_symlink(stale, alias);
    pump.prepare(service, alias, {alias / "item"});
    fs::remove(alias);
    fs::create_directory_symlink(fixture.home / "old-stale", alias);
    pump.trash(service);
    require(
        service.snapshot().results.front().outcome == files::TrashOutcome::FAILED
            && fs::exists(stale / "item")
            && fs::exists(alias / "item"),
        "redirected parent symlink was followed"
    );
  }
  void rejectsInvalidRequests(const Fixture& fixture, Pump& pump) {
    const auto parent = fixture.folder("invalid");
    write(parent / "keep", "keep");
    files::TrashService service;
    for (const auto& sources : std::vector<std::vector<files::Location>>{
             {location(parent / "keep"), location(parent / "keep")},
             {location(parent / "keep"), location(parent / "missing")},
             {location(parent)},
             {location(fixture.home / "elsewhere")},
             {files::Location::fromUri("sftp://example.invalid/file")}
         }) {
      require(service.prepare(location(parent), sources), "invalid request was not checked asynchronously");
      pump.until([&] { return !service.busy(); });
      require(service.snapshot().status == files::TrashStatus::ERROR && !service.canTrash(), "invalid target accepted");
      require(read(parent / "keep") == "keep", "failed preparation mutated a source");
    }
    require(
        service.prepare(files::Location::fromUri("sftp://example.invalid/"), {location(parent / "keep")}),
        "remote preparation refused"
    );
    pump.until([&] { return !service.busy(); });
    require(service.snapshot().status == files::TrashStatus::ERROR, "remote parent accepted");
  }
  void cancellationAndLifetime(const Fixture& fixture, Pump& pump) {
    const auto parent = fixture.folder("cancelled");
    write(parent / "keep", "keep");
    files::TrashService service;
    pump.prepare(service, parent, {parent / "keep"});
    service.cancel();
    require(!service.trash() && read(parent / "keep") == "keep", "cancelled preparation committed");
    require(service.prepare(location(parent), {location(parent / "keep")}), "preparation refused");
    require(
        !service.prepare(location(parent), {location(parent / "keep")}) && !service.trash(),
        "busy service accepted work"
    );
    service.cancel();
    pump.until([&] { return !service.busy(); });
    require(fs::exists(parent / "keep"), "cancel during preparation mutated source");
    std::vector<fs::path> paths;
    for (int index = 0; index < 128; ++index) {
      paths.push_back(parent / ("item-" + std::to_string(index)));
      write(paths.back(), "data");
    }
    pump.prepare(service, parent, paths);
    require(service.trash(), "trash refused");
    pump.until([&] { return service.completed() != 0 || !service.busy(); });
    service.cancel();
    pump.until([&] { return !service.busy(); });
    for (const auto& result : service.snapshot().results) {
      const auto path = fs::path(*result.source.nativePath());
      require(
          (result.outcome == files::TrashOutcome::TRASHED) == !fs::exists(path),
          "cancellation outcome contradicted disk"
      );
      if (result.outcome == files::TrashOutcome::TRASHED) {
        require(fixture.archived(path).size() == 1, "committed trash lost recovery metadata");
      }
    }
    service.cancel();
    require(service.snapshot().status == files::TrashStatus::FINISHED, "cancel erased actual results");
    auto destroyed = std::make_unique<files::TrashService>();
    bool notified = false;
    destroyed->setChangedCallback([&] { notified = true; });
    require(destroyed->prepare(location(parent), {location(parent / "keep")}), "lifetime preparation refused");
    notified = false;
    destroyed.reset();
    // A second request lets the same default context dispatch the retired worker.
    pump.prepare(service, parent, {parent / "keep"});
    require(!notified && fs::exists(parent / "keep"), "destroyed service invoked observer or mutated source");
    service.cancel();
  }
  void permissionFailure(const Fixture& fixture, Pump& pump) {
    if (::geteuid() == 0) {
      return;
    }
    const auto parent = fixture.folder("permission");
    write(parent / "keep", "keep");
    files::TrashService service;
    pump.prepare(service, parent, {parent / "keep"});
    fs::permissions(parent, fs::perms::owner_read | fs::perms::owner_exec);
    pump.trash(service);
    fs::permissions(parent, fs::perms::owner_all);
    require(
        service.snapshot().results.front().outcome == files::TrashOutcome::FAILED
            && read(parent / "keep") == "keep"
            && fixture.archived(parent / "keep").empty(),
        "failed trash deleted source or claimed success"
    );
  }
  void monitoredSelection(const Fixture& fixture, Pump& pump) {
    const auto parent = fixture.folder("monitored");
    write(parent / "selected", "content");
    write(parent / "remaining", "keep");
    files::DirectorySession session;
    files::DirectoryModel model;
    session.setChangedCallback([&] { model.applySnapshot(session.snapshot()); });
    session.open(location(parent));
    pump.until([&] { return model.snapshot().status == files::DirectoryStatus::READY && model.rows().size() == 2; });
    const auto selected = std::ranges::find(model.snapshot().entries, "selected", &files::DirectoryEntry::name);
    require(selected != model.snapshot().entries.end(), "selected fixture missing");
    const auto id = selected->id;
    model.select(id);
    files::TrashService service;
    pump.prepare(service, parent, {parent / "selected"});
    pump.trash(service);
    pump.until([&] { return model.rows().size() == 1 && model.entry(id) == nullptr; });
    require(model.selectedIds().empty() && read(parent / "remaining") == "keep", "trash retargeted selection");
    session.setChangedCallback({});
  }
} // namespace

int main() {
  try {
    Fixture fixture;
    Pump pump;
    preservesRecoverableContent(fixture, pump);
    rawNames(fixture, pump);
    staleTargetsAndPartialResults(fixture, pump);
    rejectsInvalidRequests(fixture, pump);
    cancellationAndLifetime(fixture, pump);
    permissionFailure(fixture, pump);
    monitoredSelection(fixture, pump);
    std::println("trash-service checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "trash-service: {}", error.what());
    return 1;
  }
}
