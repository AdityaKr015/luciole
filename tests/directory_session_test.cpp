#include "app/glib_poll_source.h"
#include "fs/directory_session.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
  using namespace std::chrono_literals;
  using files::DirectorySession;
  using files::DirectoryStatus;
  using files::Location;
  namespace fs = std::filesystem;

  void require(bool condition, const std::string& message) {
    if (!condition) {
      throw std::runtime_error(message);
    }
  }

  struct Fixture {
    fs::path path;
    Fixture() {
      GError* error = nullptr;
      const files::GString directory(g_dir_make_tmp("noctalia-files-test-XXXXXX", &error));
      if (!directory) {
        const std::string message(error->message);
        g_error_free(error);
        throw std::runtime_error(message);
      }
      path = directory.get();
    }
    ~Fixture() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void write(const std::string& name, const std::string& contents = "data") const {
      std::ofstream file(path / name);
      file << contents;
      require(file.good(), "cannot create test file");
    }
    [[nodiscard]] Location location() const { return Location::fromPath(path.native()); }
  };

  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!done()) {
        require(std::chrono::steady_clock::now() < deadline, "timed out waiting for directory state");
        once();
      }
    }

    void once() {
      std::vector<pollfd> fds;
      const auto start = m_source.addPollFds(fds);
      const int requested = m_source.pollTimeoutMs();
      const int timeout = requested < 0 ? 20 : std::min(requested, 20);
      require(::poll(fds.data(), fds.size(), timeout) >= 0, "poll failed");
      m_source.dispatch(fds, start);
    }

  private:
    GlibPollSource m_source;
  };

  const files::DirectoryEntry& entry(const DirectorySession& session, const std::string& name) {
    const auto& entries = session.snapshot().entries;
    const auto it = std::ranges::find(entries, name, &files::DirectoryEntry::name);
    require(it != entries.end(), "entry missing: " + name);
    return *it;
  }

  bool hasEntry(const DirectorySession& session, const std::string& name) {
    return std::ranges::find(session.snapshot().entries, name, &files::DirectoryEntry::name)
        != session.snapshot().entries.end();
  }

  void ready(Pump& pump, const DirectorySession& session) {
    pump.until([&session] { return session.snapshot().status != DirectoryStatus::LOADING; });
    require(
        session.snapshot().status == DirectoryStatus::READY,
        session.snapshot().error ? session.snapshot().error->message : "directory not ready"
    );
  }

  void testLocations() {
    const std::string bytes = "/tmp/a space#%\n" + std::string(1, static_cast<char>(0xFF));
    const auto location = Location::fromPath(bytes);
    require(location.nativePath() == bytes, "native bytes were changed");
    require(location.uri().contains("%23%25%0A%FF"), "filename was not URI escaped");
    require(Location::fromUri(location.uri()) == location, "URI roundtrip changed identity");
    const auto remote = Location::fromUri("sftp://example.test/folder%20name");
    require(!remote.nativePath(), "remote URI became a native path");
    for (const auto& invalid :
         std::vector<std::string>{"relative/path", "file:///bad%xy", std::string("file:///bad\0tail", 17)}) {
      bool rejected = false;
      try {
        (void)Location::fromUri(invalid);
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      require(rejected, "invalid URI accepted");
    }
  }

  void testEnumerationAndMonitoring(Pump& pump) {
    Fixture fixture;
    const std::string unusual = "raw#%\n" + std::string(1, static_cast<char>(0xFF));
    fixture.write(unusual);
    fixture.write(".hidden");
    fs::create_directory(fixture.path / "folder");
    fs::create_symlink("missing-target", fixture.path / "broken-link");
    // Cross several asynchronous batches without turning hard links into one entry.
    for (int i = 0; i < 150; ++i) {
      fixture.write(std::to_string(i));
    }
    fs::create_hard_link(fixture.path / "0", fixture.path / "hard-link");
    DirectorySession session;
    session.open(fixture.location());
    require(session.snapshot().status == DirectoryStatus::LOADING, "enumeration was synchronous");
    ready(pump, session);
    require(session.snapshot().entries.size() == 155, "incomplete batched listing");
    require(!session.snapshot().monitorError, "local monitor unavailable");
    require(entry(session, unusual).location.nativePath() == (fixture.path / unusual).native(), "raw name lost");
    require(g_utf8_validate(entry(session, unusual).displayName.c_str(), -1, nullptr), "invalid display UTF-8");
    require(entry(session, ".hidden").hidden, "hidden attribute missing");
    require(entry(session, "broken-link").symlink, "symlink followed or omitted");
    require(entry(session, "folder").type == G_FILE_TYPE_DIRECTORY, "directory type missing");
    require(entry(session, "0").id != entry(session, "hard-link").id, "hard links share an entry ID");
    const auto id = entry(session, unusual).id;
    session.refresh();
    ready(pump, session);
    require(entry(session, unusual).id == id, "refresh changed identity");
    fs::rename(fixture.path / unusual, fixture.path / "renamed");
    pump.until([&session] {
      return session.snapshot().status == DirectoryStatus::READY && hasEntry(session, "renamed");
    });
    require(entry(session, "renamed").id == id, "observed rename changed identity");
    fixture.write("renamed", "longer contents");
    pump.until([&session] { return entry(session, "renamed").size == 15; });
    require(entry(session, "renamed").id == id, "metadata change changed identity");
    fs::remove(fixture.path / "renamed");
    pump.until([&session] { return !hasEntry(session, "renamed"); });
    fixture.write("renamed");
    pump.until([&session] { return hasEntry(session, "renamed"); });
    require(entry(session, "renamed").id != id, "delete/create reused identity");
  }

  void testGenerationAndLifetime(Pump& pump) {
    Fixture first;
    Fixture second;
    first.write("first");
    second.write("second");
    DirectorySession session;
    session.open(first.location());
    const auto generation = session.snapshot().generation;
    session.open(second.location());
    ready(pump, session);
    require(session.snapshot().generation > generation, "navigation did not advance generation");
    require(!hasEntry(session, "first") && hasEntry(session, "second"), "obsolete result leaked into new location");
    session.open(first.location());
    session.cancel();
    bool drained = false;
    g_idle_add(
        +[](gpointer data) -> gboolean {
          *static_cast<bool*>(data) = true;
          return G_SOURCE_REMOVE;
        },
        &drained
    );
    pump.until([&drained] { return drained; });
    require(session.snapshot().status == DirectoryStatus::CANCELLED, "cancelled scan published results");
    session.refresh();
    ready(pump, session);
    first.write("after-resume");
    pump.until([&session] { return hasEntry(session, "after-resume"); });
    const auto beforeCancel = entry(session, "first").id;
    session.cancel();
    fs::remove(first.path / "first");
    first.write("first");
    session.refresh();
    ready(pump, session);
    require(entry(session, "first").id != beforeCancel, "unobserved replacement reused identity after cancel");

    auto dying = std::make_unique<DirectorySession>();
    dying->setChangedCallback([&dying] { dying.reset(); });
    dying->open(first.location());
    // A worker wakeup must reach the GLib fd through our poll adapter.
    bool woke = false;
    std::thread worker([&woke] {
      g_main_context_invoke(
          nullptr,
          +[](gpointer data) -> gboolean {
            *static_cast<bool*>(data) = true;
            return G_SOURCE_REMOVE;
          },
          &woke
      );
    });
    worker.join();
    pump.until([&woke] { return woke; });
  }

  void testChangesDuringLoading(Pump& pump) {
    Fixture fixture;
    fixture.write("original");
    DirectorySession session;
    session.open(fixture.location());
    ready(pump, session);
    const auto originalId = entry(session, "original").id;
    bool changed = false;
    session.setChangedCallback([&] {
      if (!changed && session.snapshot().status == DirectoryStatus::LOADING) {
        changed = true;
        fs::rename(fixture.path / "original", fixture.path / "moved");
        fixture.write("during-load");
      }
    });
    session.refresh();
    pump.until([&session] {
      return session.snapshot().status == DirectoryStatus::READY
          && hasEntry(session, "moved")
          && hasEntry(session, "during-load")
          && !hasEntry(session, "original");
    });
    require(entry(session, "moved").id == originalId, "rename while loading lost identity");

    // An atomic replacement must invalidate the target's old identity, even
    // when the temporary source never appeared in a published listing.
    fixture.write("replacement-tmp", "replacement");
    fs::rename(fixture.path / "replacement-tmp", fixture.path / "moved");
    pump.until([&session, originalId] {
      return entry(session, "moved").id != originalId && entry(session, "moved").size == 11;
    });
    session.setChangedCallback({});
    fs::remove_all(fixture.path);
    pump.until([&session] { return session.snapshot().status == DirectoryStatus::ERROR; });
    require(session.snapshot().error->kind == files::FileErrorKind::MISSING, "deleted directory left a stale listing");
    require(session.snapshot().entries.empty(), "deleted directory retained entries");
  }

  void testErrorsAndEmpty(Pump& pump) {
    Fixture fixture;
    DirectorySession session;
    session.open(fixture.location());
    ready(pump, session);
    require(session.snapshot().entries.empty(), "empty directory not ready and empty");
    session.open(Location::fromPath((fixture.path / "missing").native()));
    pump.until([&session] { return session.snapshot().status == DirectoryStatus::ERROR; });
    require(session.snapshot().error->kind == files::FileErrorKind::MISSING, "missing location not reported");
    fixture.write("plain-file");
    session.open(Location::fromPath((fixture.path / "plain-file").native()));
    pump.until([&session] { return session.snapshot().status == DirectoryStatus::ERROR; });
    require(session.snapshot().error->kind == files::FileErrorKind::NOT_DIRECTORY, "file location error lost");
  }
} // namespace

int main() {
  try {
    testLocations();
    Pump pump;
    testEnumerationAndMonitoring(pump);
    testGenerationAndLifetime(pump);
    testChangesDuringLoading(pump);
    testErrorsAndEmpty(pump);
    std::println("directory session checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "directory session test: {}", error.what());
    return 1;
  }
}
