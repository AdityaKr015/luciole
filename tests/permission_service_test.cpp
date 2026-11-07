#include "app/glib_poll_source.h"
#include "operations/permission_service.h"
#include "test_check.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using S = files::PermissionService;
  using P = files::PermissionPhase;
  struct Fixture {
    fs::path root;
    Fixture() {
      std::string path = "/tmp/noctalia-permissions-XXXXXX";
      TEST_CHECK(::mkdtemp(path.data()));
      root = path;
    }
    ~Fixture() {
      std::error_code error;
      fs::remove_all(root, error);
    }
  };
  void write(const fs::path& path) { std::ofstream(path) << "payload"; }
  struct stat metadata(const fs::path& path) {
    struct stat value{};
    TEST_CHECK(::lstat(path.c_str(), &value) == 0);
    return value;
  }
  unsigned mode(const fs::path& path) { return metadata(path).st_mode & 07777; }
  files::DirectoryEntry entry(const fs::path& path) {
    const auto location = files::Location::fromPath(path.native());
    const files::GioPtr<GFileInfo> info(g_file_query_info(
        location.file().get(), "id::file,id::filesystem,standard::type,standard::is-symlink",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    TEST_CHECK(info);
    const files::GString display(g_filename_display_name(path.filename().c_str()));
    return {
        .id = 1,
        .location = location,
        .name = path.filename().native(),
        .displayName = display.get(),
        .type = g_file_info_get_file_type(info.get()),
        .symlink = g_file_info_get_is_symlink(info.get()) != FALSE,
        .size = {},
        .modifiedSeconds = {},
        .etag = {},
        .parentDisplayName = {},
        .identity = files::readFileIdentity(info.get())
    };
  }
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto first = m_source.addPollFds(fds);
        TEST_CHECK(::poll(fds.data(), fds.size(), 1) >= 0);
        m_source.dispatch(fds, first);
      }
    }
    void settle(S& service) {
      until([&] { return !service.busy(); });
    }
    void prepare(S& service, const fs::path& path) {
      TEST_CHECK(service.prepare(entry(path)));
      settle(service);
      TEST_CHECK(service.snapshot().phase == P::Ready && service.snapshot().info);
    }
    void apply(S& service, files::PermissionEdit edit) {
      TEST_CHECK(service.apply(std::move(edit)));
      settle(service);
      TEST_CHECK(service.snapshot().phase == P::Ready);
    }

  private:
    GlibPollSource m_source;
  };
  void modesAndAliases(Pump& pump) {
    Fixture f;
    S service;
    const auto file = f.root / "file";
    write(file);
    TEST_CHECK(::chmod(file.c_str(), 0640) == 0);
    pump.prepare(service, file);
    TEST_CHECK(service.snapshot().info->mode == 0640 && service.snapshot().info->owner == ::geteuid());
    TEST_CHECK(service.snapshot().info->canMode && service.snapshot().info->canGroup);
    TEST_CHECK(service.snapshot().info->canOwner == (::geteuid() == 0));
    for (unsigned value : {0600U, 0755U, 04750U, 02750U, 01750U, 0000U, 0644U}) {
      pump.apply(service, {.mode = value, .owner = {}, .group = {}});
      TEST_CHECK(!service.snapshot().error && mode(file) == value && service.snapshot().info->mode == value);
    }
    pump.apply(service, {.mode = 0644, .owner = {}, .group = {}});
    TEST_CHECK(!service.snapshot().applied && !service.snapshot().error);
    fs::create_hard_link(file, f.root / "alias");
    pump.prepare(service, file);
    TEST_CHECK(service.snapshot().info->multipleLinks);
    pump.apply(service, {.mode = 0600, .owner = {}, .group = {}});
    TEST_CHECK(mode(f.root / "alias") == 0600);
    fs::create_directory(f.root / "folder");
    write(f.root / "folder/child");
    const auto childMode = mode(f.root / "folder/child");
    pump.prepare(service, f.root / "folder");
    pump.apply(service, {.mode = 01750, .owner = {}, .group = {}});
    TEST_CHECK(mode(f.root / "folder") == 01750 && mode(f.root / "folder/child") == childMode);
    TEST_CHECK(::mkfifo((f.root / "pipe").c_str(), 0600) == 0);
    pump.prepare(service, f.root / "pipe");
    pump.apply(service, {.mode = 0640, .owner = {}, .group = {}});
    TEST_CHECK(mode(f.root / "pipe") == 0640);
    const std::string raw = std::string("raw-") + static_cast<char>(0xff);
    write(f.root / raw);
    pump.prepare(service, f.root / raw);
    pump.apply(service, {.mode = 0600, .owner = {}, .group = {}});
    TEST_CHECK(mode(f.root / raw) == 0600);
  }
  void ownershipAndValidation(Pump& pump) {
    Fixture f;
    S service;
    const auto file = f.root / "file";
    write(file);
    pump.prepare(service, file);
    pump.apply(service, {.mode = {}, .owner = std::to_string(::geteuid()), .group = std::to_string(::getegid())});
    TEST_CHECK(!service.snapshot().error && !service.snapshot().applied);
    const auto info = *service.snapshot().info;
    if (!info.ownerName.empty() && !info.groupName.empty()) {
      pump.apply(service, {.mode = {}, .owner = info.ownerName, .group = info.groupName});
      TEST_CHECK(!service.snapshot().error && !service.snapshot().applied);
    }
    const auto original = mode(file);
    for (const std::string value : {"", "4294967295", "999999999999999999999", "noctalia-no-such-account-370917"}) {
      pump.apply(service, {.mode = 0777, .owner = {}, .group = value});
      TEST_CHECK(service.snapshot().error && !service.snapshot().applied && mode(file) == original);
    }
    pump.apply(service, {.mode = 010000, .owner = {}, .group = {}});
    TEST_CHECK(service.snapshot().error && mode(file) == original);
    if (::geteuid() != 0) {
      pump.apply(service, {.mode = 0777, .owner = "0", .group = {}});
      TEST_CHECK(service.snapshot().error && metadata(file).st_uid == ::geteuid() && mode(file) == original);
      pump.apply(service, {.mode = 0777, .owner = {}, .group = "4294967294"});
      TEST_CHECK(service.snapshot().error && mode(file) == original);
    }
    pump.apply(service, {.mode = 0600, .owner = {}, .group = {}});
    TEST_CHECK(!service.snapshot().error && mode(file) == 0600);
    const int count = ::getgroups(0, nullptr);
    TEST_CHECK(count >= 0);
    std::vector<gid_t> groups(static_cast<std::size_t>(count));
    TEST_CHECK(::getgroups(count, groups.data()) >= 0);
    for (const auto group : groups) {
      if (group == metadata(file).st_gid) {
        continue;
      }
      TEST_CHECK(::chmod(file.c_str(), 04750) == 0);
      pump.prepare(service, file);
      pump.apply(service, {.mode = {}, .owner = {}, .group = std::to_string(group)});
      TEST_CHECK(!service.snapshot().error && service.snapshot().applied && metadata(file).st_gid == group);
      TEST_CHECK(service.snapshot().info->mode == mode(file) && (mode(file) & 04000) == 0);
      pump.apply(service, {.mode = 0640, .owner = {}, .group = std::to_string(::getegid())});
      TEST_CHECK(!service.snapshot().error && metadata(file).st_gid == ::getegid() && mode(file) == 0640);
      std::println("Verified membership-based group changes and kernel set-ID clearing.");
      break;
    }
  }
  void linksAndStaleItems(Pump& pump) {
    Fixture f;
    S service;
    const auto file = f.root / "file";
    write(file);
    TEST_CHECK(::chmod(file.c_str(), 0600) == 0);
    fs::create_symlink("file", f.root / "link");
    pump.prepare(service, f.root / "link");
    TEST_CHECK(service.snapshot().info->symlink && !service.snapshot().info->canMode);
    pump.apply(service, {.mode = 0777, .owner = {}, .group = {}});
    TEST_CHECK(service.snapshot().error && mode(file) == 0600 && fs::is_symlink(f.root / "link"));
    fs::create_symlink("missing", f.root / "broken");
    pump.prepare(service, f.root / "broken");
    pump.apply(service, {.mode = {}, .owner = std::to_string(::geteuid()), .group = std::to_string(::getegid())});
    TEST_CHECK(!service.snapshot().error && fs::read_symlink(f.root / "broken") == "missing");
    const int count = ::getgroups(0, nullptr);
    TEST_CHECK(count >= 0);
    std::vector<gid_t> groups(static_cast<std::size_t>(count));
    TEST_CHECK(::getgroups(count, groups.data()) >= 0);
    const auto targetGroup = metadata(file).st_gid;
    for (const auto group : groups) {
      if (group == targetGroup) {
        continue;
      }
      for (const auto* name : {"link", "broken"}) {
        pump.prepare(service, f.root / name);
        pump.apply(service, {.mode = {}, .owner = {}, .group = std::to_string(group)});
        TEST_CHECK(!service.snapshot().error && metadata(f.root / name).st_gid == group);
        TEST_CHECK(metadata(file).st_gid == targetGroup && mode(file) == 0600);
      }
      std::println("Verified descriptor-based ownership changes on live and broken links.");
      break;
    }
    const auto captured = entry(file);
    fs::rename(file, f.root / "held");
    write(file);
    TEST_CHECK(service.prepare(captured));
    pump.settle(service);
    TEST_CHECK(service.snapshot().error);
    pump.prepare(service, file);
    fs::rename(file, f.root / "held2");
    write(file);
    const auto replacementMode = mode(file);
    pump.apply(service, {.mode = 0777, .owner = {}, .group = {}});
    TEST_CHECK(
        service.snapshot().error
        && !service.snapshot().applied
        && mode(file) == replacementMode
        && mode(f.root / "held2") == replacementMode
    );
    pump.prepare(service, file);
    TEST_CHECK(::chmod(file.c_str(), 0600) == 0);
    pump.apply(service, {.mode = 0777, .owner = {}, .group = {}});
    TEST_CHECK(
        service.snapshot().error
        && !service.snapshot().applied
        && mode(file) == 0600
        && !service.snapshot().info->canMode
    );
    fs::create_directory(f.root / "folder");
    write(f.root / "folder/item");
    pump.prepare(service, f.root / "folder/item");
    fs::rename(f.root / "folder", f.root / "old-folder");
    fs::create_directory(f.root / "folder");
    write(f.root / "folder/item");
    pump.apply(service, {.mode = 0777, .owner = {}, .group = {}});
    TEST_CHECK(
        service.snapshot().error && mode(f.root / "folder/item") != 0777 && mode(f.root / "old-folder/item") != 0777
    );
    fs::create_directory_symlink(f.root / "old-folder", f.root / "parent-link");
    pump.prepare(service, f.root / "parent-link/item");
    fs::remove(f.root / "parent-link");
    fs::create_directory_symlink(f.root / "folder", f.root / "parent-link");
    pump.apply(service, {.mode = 0777, .owner = {}, .group = {}});
    TEST_CHECK(service.snapshot().error && mode(f.root / "folder/item") != 0777);
  }
  void cancellationAndLifetime(Pump& pump) {
    Fixture f;
    S service;
    const auto file = f.root / "file";
    write(file);
    TEST_CHECK(::chmod(file.c_str(), 0600) == 0);
    TEST_CHECK(service.prepare(entry(file)));
    service.cancel();
    pump.settle(service);
    TEST_CHECK(service.snapshot().phase == P::Cancelled && mode(file) == 0600);
    pump.prepare(service, file);
    TEST_CHECK(service.apply({.mode = 0644, .owner = {}, .group = {}}));
    TEST_CHECK(!service.prepare(entry(file)) && !service.apply({}));
    service.cancel();
    pump.settle(service);
    TEST_CHECK(service.snapshot().info && service.snapshot().info->mode == mode(file));
    TEST_CHECK(
        (mode(file) == 0600 && !service.snapshot().applied) || (mode(file) == 0644 && service.snapshot().applied)
    );
    bool notified = false;
    {
      auto temporary = std::make_unique<S>();
      temporary->setChangedCallback([&] { notified = true; });
      TEST_CHECK(temporary->prepare(entry(file)));
      temporary.reset();
      notified = false;
    }
    pump.prepare(service, file);
    TEST_CHECK(!notified);
    auto unknown = entry(file);
    unknown.identity.reset();
    TEST_CHECK(service.prepare(unknown));
    pump.settle(service);
    TEST_CHECK(
        service.snapshot().info
        && !service.snapshot().info->canMode
        && !service.apply({.mode = 0777, .owner = {}, .group = {}})
    );
    TEST_CHECK(service.prepare(files::Location::fromPath(file.native())));
    pump.settle(service);
    TEST_CHECK(service.snapshot().info && service.snapshot().info->canMode);
    pump.apply(service, {.mode = 0640, .owner = {}, .group = {}});
    TEST_CHECK(!service.snapshot().error && mode(file) == 0640);
    auto remote = entry(file);
    remote.location = files::Location::fromUri("sftp://example.invalid/file");
    TEST_CHECK(service.prepare(remote));
    pump.settle(service);
    TEST_CHECK(service.snapshot().phase == P::Error);
    TEST_CHECK(mode(file) != 0777);
  }
} // namespace
int main() {
  Pump pump;
  modesAndAliases(pump);
  ownershipAndValidation(pump);
  linksAndStaleItems(pump);
  cancellationAndLifetime(pump);
  std::println("permission service checks passed");
}
