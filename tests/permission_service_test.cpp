#include "app/glib_poll_source.h"
#include "operations/permission_service.h"
#include "test_check.h"

#include <acl/libacl.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <set>
#include <sys/acl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using S = files::PermissionService;
  using P = files::PermissionPhase;
  unsigned supportedSpecialBits = 0;
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
  unsigned probeSpecialBits() {
    Fixture fixture;
    const auto file = fixture.root / "mode-probe";
    write(file);
    TEST_CHECK(::chmod(file.c_str(), 0600) == 0);
    unsigned supported = 0;
    for (const unsigned bit : {04000U, 02000U, 01000U}) {
      errno = 0;
      if (::chmod(file.c_str(), 0600 | bit) == 0) {
        TEST_CHECK(mode(file) == (0600 | bit));
        supported |= bit;
      } else {
        TEST_CHECK(errno == EPERM || errno == EOPNOTSUPP);
        TEST_CHECK(mode(file) == 0600);
      }
      TEST_CHECK(::chmod(file.c_str(), 0600) == 0);
    }
    std::println("Fixture special-bit capabilities: {:04o}", supported);
    return supported;
  }
  unsigned fixtureMode(unsigned value) { return value & (0777 | supportedSpecialBits); }
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
      const auto before = mode(file);
      pump.apply(service, {.mode = value, .owner = {}, .group = {}});
      if ((value & 07000 & ~supportedSpecialBits) != 0) {
        TEST_CHECK(service.snapshot().error && !service.snapshot().applied && mode(file) == before);
        TEST_CHECK(service.snapshot().info->mode == before);
      } else {
        TEST_CHECK(!service.snapshot().error && mode(file) == value && service.snapshot().info->mode == value);
      }
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
      TEST_CHECK(::chmod(file.c_str(), fixtureMode(04750)) == 0);
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
  using O = files::PermissionOutcome;
  void prepareBulk(Pump& pump, S& service, std::vector<files::DirectoryEntry> entries) {
    TEST_CHECK(service.prepare(std::move(entries)));
    pump.settle(service);
    TEST_CHECK(service.snapshot().phase == P::Ready && service.snapshot().multiple && service.snapshot().results);
  }
  void bulkMasksAndOwnership(Pump& pump) {
    Fixture f;
    S service;
    const auto a = f.root / "a", b = f.root / "b";
    write(a);
    write(b);
    TEST_CHECK(::chmod(a.c_str(), fixtureMode(04760)) == 0 && ::chmod(b.c_str(), fixtureMode(02755)) == 0);
    prepareBulk(pump, service, {entry(a), entry(b)});
    TEST_CHECK(service.snapshot().mixedMode == (fixtureMode(04760) ^ fixtureMode(02755)));
    TEST_CHECK(!service.snapshot().mixedOwner && !service.snapshot().mixedGroup);
    pump.apply(service, {.mode = 0100, .owner = {}, .group = {}, .mask = 0104});
    TEST_CHECK(mode(a) == fixtureMode(04760) && mode(b) == fixtureMode(02751));
    TEST_CHECK(service.snapshot().results->front().outcome == O::Unchanged);
    TEST_CHECK(service.snapshot().results->back().outcome == O::Changed);
    TEST_CHECK(service.snapshot().report.contains(std::format("mode {:04o}", fixtureMode(02751))));
    pump.apply(service, {});
    TEST_CHECK(!service.snapshot().applied && !service.snapshot().error);
    for (const auto& row : *service.snapshot().results) {
      TEST_CHECK(row.outcome == O::Unchanged);
    }
    pump.apply(service, {.mode = 07777, .owner = {}, .group = "noctalia-no-such-group-370917", .mask = 07777});
    TEST_CHECK(service.snapshot().error && !service.snapshot().finished && !service.snapshot().applied);
    TEST_CHECK(service.snapshot().report.starts_with("No changes attempted:"));
    for (const auto& row : *service.snapshot().results) {
      TEST_CHECK(row.outcome == O::Ready);
    }
    TEST_CHECK(mode(a) == fixtureMode(04760) && mode(b) == fixtureMode(02751));
    pump.apply(service, {.mode = 0004, .owner = {}, .group = {}, .mask = 0004});
    TEST_CHECK(!service.snapshot().error && mode(a) == fixtureMode(04764) && mode(b) == fixtureMode(02755));
    const int count = ::getgroups(0, nullptr);
    TEST_CHECK(count >= 0);
    std::vector<gid_t> groups(static_cast<std::size_t>(count));
    TEST_CHECK(::getgroups(count, groups.data()) >= 0);
    for (const auto group : groups) {
      if (group == metadata(a).st_gid) {
        continue;
      }
      pump.apply(service, {.mode = 0004, .owner = {}, .group = std::to_string(group), .mask = 0004});
      TEST_CHECK(!service.snapshot().error && metadata(a).st_gid == group && metadata(b).st_gid == group);
      TEST_CHECK(mode(a) == 0764 && mode(b) == 0755); // Keep does not restore kernel-cleared set-ID bits.
      std::println("Verified bulk group changes preserve kernel set-ID clearing.");
      break;
    }
    fs::create_directory(f.root / "folder");
    write(f.root / "folder/child");
    const auto child = mode(f.root / "folder/child");
    prepareBulk(pump, service, {entry(f.root / "folder"), entry(a)});
    pump.apply(service, {.mode = 0, .owner = {}, .group = {}, .mask = 0007});
    TEST_CHECK((mode(f.root / "folder") & 0007) == 0 && mode(f.root / "folder/child") == child);
  }
  void bulkFailuresAndAliases(Pump& pump) {
    Fixture f;
    S service;
    const auto a = f.root / "a", b = f.root / "b";
    write(a);
    write(b);
    TEST_CHECK(::chmod(a.c_str(), 0600) == 0 && ::chmod(b.c_str(), 0600) == 0);
    prepareBulk(pump, service, {entry(a), entry(b)});
    fs::rename(a, f.root / "held");
    write(a);
    const auto replacement = mode(a);
    pump.apply(service, {.mode = 0004, .owner = {}, .group = {}, .mask = 0004});
    TEST_CHECK(service.snapshot().error && service.snapshot().applied && service.snapshot().finished);
    TEST_CHECK(mode(a) == replacement && mode(f.root / "held") == 0600 && mode(b) == 0604);
    TEST_CHECK(
        service.snapshot().results->front().outcome == O::Failed
        && service.snapshot().results->back().outcome == O::Changed
    );
    auto remote = entry(a);
    remote.location = files::Location::fromUri("sftp://example.invalid/file");
    auto unknown = entry(b);
    unknown.identity.reset();
    prepareBulk(pump, service, {entry(a), unknown, remote});
    TEST_CHECK(service.snapshot().unknownItems == 1 && service.snapshot().results->back().outcome == O::Failed);
    pump.apply(service, {.mode = 0002, .owner = {}, .group = {}, .mask = 0002});
    TEST_CHECK(service.snapshot().error && mode(a) == (replacement | 0002) && mode(b) == 0604);
    TEST_CHECK(service.snapshot().results->at(1).outcome == O::Failed);
    fs::create_symlink("a", f.root / "link");
    prepareBulk(pump, service, {entry(b), entry(f.root / "link")});
    pump.apply(service, {.mode = 0001, .owner = {}, .group = {}, .mask = 0001});
    TEST_CHECK(service.snapshot().error && mode(a) == (replacement | 0002) && mode(b) == 0605);
    TEST_CHECK(service.snapshot().results->back().outcome == O::Failed);
    fs::create_hard_link(b, f.root / "alias");
    prepareBulk(pump, service, {entry(b), entry(f.root / "alias")});
    pump.apply(service, {.mode = 0002, .owner = {}, .group = {}, .mask = 0002});
    TEST_CHECK(!service.snapshot().error && mode(b) == 0607);
    TEST_CHECK(
        service.snapshot().results->front().outcome == O::Changed
        && service.snapshot().results->back().outcome == O::Changed
    );
    TEST_CHECK(service.snapshot().results->back().message.contains("hard link"));
    pump.apply(service, {.mode = 0, .owner = {}, .group = {}, .mask = 0002});
    TEST_CHECK(!service.snapshot().error && mode(b) == 0605);
    prepareBulk(pump, service, {entry(b), entry(f.root / "alias")});
    fs::remove(f.root / "alias");
    write(f.root / "alias");
    const auto aliasMode = mode(f.root / "alias");
    pump.apply(service, {.mode = 0020, .owner = {}, .group = {}, .mask = 0020});
    TEST_CHECK(service.snapshot().error && mode(f.root / "alias") == aliasMode && mode(b) == 0625);
    fs::remove(f.root / "alias");
    fs::create_hard_link(b, f.root / "alias");
    pump.apply(service, {.mode = 0020, .owner = {}, .group = {}, .mask = 0020});
    TEST_CHECK(service.snapshot().error && !service.snapshot().applied);
    TEST_CHECK(service.snapshot().results->back().outcome == O::Failed);
    prepareBulk(pump, service, {entry(a), entry(b)});
    TEST_CHECK(::chmod(a.c_str(), 0400) == 0);
    pump.apply(service, {.mode = 0010, .owner = {}, .group = {}, .mask = 0010});
    TEST_CHECK(service.snapshot().error && mode(a) == 0400 && mode(b) == 0635);
    const auto source = entry(a);
    TEST_CHECK(!service.prepare(std::vector<files::DirectoryEntry>{}));
    TEST_CHECK(!service.prepare(std::vector{source}));
    TEST_CHECK(!service.prepare(std::vector<files::DirectoryEntry>(4097, source)));
    TEST_CHECK(service.prepare(std::vector{source, source}));
    pump.settle(service);
    TEST_CHECK(service.snapshot().phase == P::Error);
  }
  void bulkCancellation(Pump& pump) {
    for (const bool aliases : {false, true}) {
      Fixture f;
      S service;
      constexpr std::size_t count = 256;
      std::vector<files::DirectoryEntry> entries;
      write(f.root / "original");
      TEST_CHECK(::chmod((f.root / "original").c_str(), 0600) == 0);
      for (std::size_t i = 0; i < count; ++i) {
        const auto path = f.root / std::to_string(i);
        if (aliases) {
          fs::create_hard_link(f.root / "original", path);
        } else {
          write(path);
          TEST_CHECK(::chmod(path.c_str(), 0600) == 0);
        }
        entries.push_back(entry(path));
      }
      prepareBulk(pump, service, entries);
      TEST_CHECK(service.apply({.mode = 0004, .owner = {}, .group = {}, .mask = 0004}));
      pump.until([&] { return service.completed() > 0 || !service.busy(); });
      if (service.busy()) {
        service.cancel();
      }
      pump.settle(service);
      TEST_CHECK(service.snapshot().finished && service.snapshot().applied);
      std::size_t changed = 0;
      for (const auto& row : *service.snapshot().results) {
        TEST_CHECK(row.info && row.info->mode == mode(*row.location.nativePath()));
        TEST_CHECK(row.outcome == O::Changed || row.outcome == O::Cancelled);
        if (row.outcome == O::Changed) {
          ++changed;
        }
        if (aliases) {
          TEST_CHECK(row.outcome == O::Changed && row.info->mode == 0604);
        }
      }
      TEST_CHECK(changed > 0 && changed <= count);
      std::println("Bulk cancellation: aliases={}, changed={} of {}", aliases, changed, count);
      TEST_CHECK(service.prepare(entries));
      service.cancel();
      pump.settle(service);
      TEST_CHECK(service.snapshot().phase == P::Cancelled);
    }
  }
  void bulkOwnershipCancellation(Pump& pump) {
    const int count = ::getgroups(0, nullptr);
    TEST_CHECK(count >= 0);
    std::vector<gid_t> groups(static_cast<std::size_t>(count));
    TEST_CHECK(::getgroups(count, groups.data()) >= 0);
    const auto other = std::ranges::find_if(groups, [](gid_t group) { return group != ::getegid(); });
    if (other == groups.end()) {
      return;
    }
    Fixture f;
    S service;
    std::vector<files::DirectoryEntry> entries;
    for (std::size_t i = 0; i < 256; ++i) {
      const auto path = f.root / std::to_string(i);
      write(path);
      TEST_CHECK(::chmod(path.c_str(), fixtureMode(04750)) == 0);
      entries.push_back(entry(path));
    }
    prepareBulk(pump, service, entries);
    TEST_CHECK(service.apply({.mode = 0004, .owner = {}, .group = std::to_string(*other), .mask = 0004}));
    pump.until([&] { return service.completed() > 0 || !service.busy(); });
    if (service.busy()) {
      service.cancel();
    }
    pump.settle(service);
    TEST_CHECK(service.snapshot().finished);
    for (const auto& row : *service.snapshot().results) {
      const auto value = metadata(*row.location.nativePath());
      TEST_CHECK(row.info && row.info->mode == (value.st_mode & 07777) && row.info->group == value.st_gid);
      const bool full = value.st_gid == *other && (value.st_mode & 0004) != 0;
      const bool untouched = value.st_gid == ::getegid() && (value.st_mode & 07777) == fixtureMode(04750);
      TEST_CHECK(
          full            ? row.outcome == O::Changed
              : untouched ? row.outcome == O::Cancelled
                          : row.outcome == O::Partial
      );
    }
  }
  using O = files::PermissionOutcome;
  files::RecursivePermissionEdit recursiveEdit() {
    return {
        .files = {.mode = 0004, .owner = {}, .group = {}, .mask = 0004}, .directoryMode = 0010, .directoryMask = 0010
    };
  }
  void recursiveModes(Pump& pump) {
    Fixture f;
    const auto tree = f.root / "tree";
    fs::create_directories(tree / "child/nested");
    const auto file = tree / "child/file";
    const auto nested = tree / "child/nested/value";
    write(file);
    write(nested);
    write(tree / ".hidden");
    write(f.root / "outside");
    TEST_CHECK(::chmod(file.c_str(), 0640) == 0 && ::chmod(nested.c_str(), 0600) == 0);
    TEST_CHECK(::chmod((tree / ".hidden").c_str(), 0600) == 0);
    fs::create_hard_link(file, tree / "alias");
    fs::create_symlink(f.root / "outside", tree / "external-link");
    fs::create_directory_symlink(tree, tree / "cycle");
    fs::create_symlink(tree / "missing", tree / "broken-link");
    TEST_CHECK(::mkfifo((tree / "fifo").c_str(), 0600) == 0);
    const std::string raw = std::string("raw-") + static_cast<char>(0xff);
    write(tree / raw);
    const auto outsideMode = mode(f.root / "outside");
    S service;
    // A descendant selected before its ancestor is still processed exactly once.
    TEST_CHECK(service.prepare(std::vector{entry(file), entry(tree / "child"), entry(tree)}));
    pump.settle(service);
    TEST_CHECK(service.applyRecursive(recursiveEdit()));
    pump.settle(service);
    const auto& snapshot = service.snapshot();
    TEST_CHECK(snapshot.recursive && snapshot.finished && !snapshot.error && snapshot.applied);
    TEST_CHECK(mode(file) == 0644 && mode(nested) == 0604 && mode(tree / ".hidden") == 0604);
    TEST_CHECK(mode(tree / "fifo") == 0604 && mode(f.root / "outside") == outsideMode);
    TEST_CHECK(mode(tree / "alias") == 0644);
    std::set<std::string> names;
    std::size_t links = 0;
    for (const auto& row : *snapshot.results) {
      TEST_CHECK(names.insert(row.location.uri()).second);
      if (row.outcome == O::Skipped) {
        ++links;
      } else {
        TEST_CHECK(row.info && row.info->mode == mode(*row.location.nativePath()));
      }
    }
    TEST_CHECK(links == 3 && snapshot.results->size() == 12);
    TEST_CHECK(!service.applyRecursive(recursiveEdit()));
    // Closing parent access must happen after the files have been changed.
    pump.prepare(service, tree);
    auto edit = recursiveEdit();
    edit.files.mode = 0;
    edit.files.mask = 0004;
    edit.directoryMode = 0;
    edit.directoryMask = 0777;
    TEST_CHECK(service.applyRecursive(edit));
    pump.settle(service);
    TEST_CHECK(!service.snapshot().error && mode(tree) == 0);
    TEST_CHECK(::chmod(tree.c_str(), 0700) == 0);
    TEST_CHECK(::chmod((tree / "child").c_str(), 0700) == 0);
    TEST_CHECK(::chmod((tree / "child/nested").c_str(), 0700) == 0);
    TEST_CHECK(mode(file) == 0640 && mode(nested) == 0600);
  }
  void recursiveFailures(Pump& pump) {
    Fixture f;
    fs::create_directories(f.root / "tree/blocked");
    write(f.root / "tree/blocked/value");
    write(f.root / "tree/good");
    TEST_CHECK(::chmod((f.root / "tree/good").c_str(), 0600) == 0);
    S service;
    pump.prepare(service, f.root / "tree");
    auto invalid = recursiveEdit();
    invalid.directoryMode = 010000;
    TEST_CHECK(service.applyRecursive(invalid));
    pump.settle(service);
    TEST_CHECK(service.snapshot().error && !service.snapshot().applied && !service.snapshot().finished);
    TEST_CHECK(mode(f.root / "tree/good") == 0600);
    invalid = recursiveEdit();
    invalid.files.group = "noctalia-no-such-account-370917";
    TEST_CHECK(service.applyRecursive(invalid));
    pump.settle(service);
    TEST_CHECK(service.snapshot().error && !service.snapshot().applied && mode(f.root / "tree/good") == 0600);
    if (::geteuid() != 0) {
      TEST_CHECK(::chmod((f.root / "tree/blocked").c_str(), 0000) == 0);
      TEST_CHECK(service.applyRecursive(recursiveEdit()));
      pump.settle(service);
      TEST_CHECK(service.snapshot().error && service.snapshot().applied);
      TEST_CHECK(mode(f.root / "tree/good") == 0604);
      TEST_CHECK(std::ranges::any_of(*service.snapshot().results, [](const auto& row) {
        return row.outcome == O::Failed && row.message.contains("Contents not inspected");
      }));
      TEST_CHECK(::chmod((f.root / "tree/blocked").c_str(), 0700) == 0);
    }
    // A captured root replaced before Apply must never be recaptured by traversal.
    pump.prepare(service, f.root / "tree");
    fs::rename(f.root / "tree", f.root / "held");
    fs::create_directory(f.root / "replacement");
    write(f.root / "replacement/value");
    const auto before = mode(f.root / "replacement/value");
    fs::create_directory_symlink(f.root / "replacement", f.root / "tree");
    TEST_CHECK(service.applyRecursive(recursiveEdit()));
    pump.settle(service);
    TEST_CHECK(service.snapshot().error && !service.snapshot().applied && mode(f.root / "replacement/value") == before);
  }
  void recursiveCancellationAndLimits(Pump& pump) {
    Fixture f;
    fs::create_directory(f.root / "tree");
    for (unsigned i = 0; i < 1024; ++i) {
      const auto file = f.root / "tree" / std::to_string(i);
      write(file);
      TEST_CHECK(::chmod(file.c_str(), 0600) == 0);
    }
    S service;
    pump.prepare(service, f.root / "tree");
    TEST_CHECK(service.applyRecursive(recursiveEdit()));
    service.cancel();
    pump.settle(service);
    TEST_CHECK(service.snapshot().phase == P::Cancelled && !service.snapshot().applied);
    for (unsigned i = 0; i < 1024; ++i) {
      TEST_CHECK(mode(f.root / "tree" / std::to_string(i)) == 0600);
    }
    // Exhausting the collection's descriptor budget must mutate nothing.
    pump.prepare(service, f.root / "tree");
    struct rlimit saved{};
    TEST_CHECK(::getrlimit(RLIMIT_NOFILE, &saved) == 0);
    auto limited = saved;
    limited.rlim_cur = std::min<rlim_t>(saved.rlim_cur, 128);
    TEST_CHECK(::setrlimit(RLIMIT_NOFILE, &limited) == 0);
    TEST_CHECK(service.applyRecursive(recursiveEdit()));
    pump.settle(service);
    TEST_CHECK(::setrlimit(RLIMIT_NOFILE, &saved) == 0);
    TEST_CHECK(service.snapshot().error && !service.snapshot().applied && service.snapshot().finished);
    TEST_CHECK(service.snapshot().message.contains("file descriptors"));
    for (unsigned i = 0; i < 1024; ++i) {
      TEST_CHECK(mode(f.root / "tree" / std::to_string(i)) == 0600);
    }
    // Cancel after collection. Completed effects stay represented in the report.
    pump.prepare(service, f.root / "tree");
    TEST_CHECK(service.applyRecursive(recursiveEdit()));
    pump.until([&] { return !service.collecting() || !service.busy(); });
    if (service.busy()) {
      service.cancel();
    }
    write(f.root / "tree/late-arrival");
    const auto lateMode = mode(f.root / "tree/late-arrival");
    pump.settle(service);
    TEST_CHECK(service.snapshot().finished && service.snapshot().results);
    for (const auto& row : *service.snapshot().results) {
      if (row.info) {
        TEST_CHECK(row.info->mode == mode(*row.location.nativePath()));
      }
      if (row.outcome == O::Changed && row.info && !row.info->directory) {
        TEST_CHECK(row.info->mode == 0604);
      }
      if (row.outcome == O::Cancelled && row.info && !row.info->directory) {
        TEST_CHECK(row.info->mode == 0600);
      }
    }
    TEST_CHECK(mode(f.root / "tree/late-arrival") == lateMode);
    auto folder = f.root / "deep";
    fs::create_directory(folder);
    for (unsigned i = 0; i < 128; ++i) {
      folder /= "d";
      fs::create_directory(folder);
    }
    pump.prepare(service, f.root / "deep");
    TEST_CHECK(service.applyRecursive(recursiveEdit()));
    pump.settle(service);
    TEST_CHECK(service.snapshot().error && !service.snapshot().applied && service.snapshot().message.contains("128"));
  }
  void recursiveOwnership(Pump& pump) {
    Fixture f;
    fs::create_directories(f.root / "tree/child");
    write(f.root / "tree/child/value");
    const auto file = f.root / "tree/child/value";
    const int count = ::getgroups(0, nullptr);
    TEST_CHECK(count >= 0);
    std::vector<gid_t> groups(static_cast<std::size_t>(count));
    TEST_CHECK(::getgroups(count, groups.data()) >= 0);
    groups.insert(groups.begin(), ::getegid());
    S service;
    for (const auto group : groups) {
      TEST_CHECK(::chmod(file.c_str(), fixtureMode(04750)) == 0);
      pump.prepare(service, f.root / "tree");
      const auto oldGroup = metadata(file).st_gid;
      auto edit = recursiveEdit();
      edit.files.owner = std::to_string(::geteuid());
      edit.files.group = std::to_string(group);
      TEST_CHECK(service.applyRecursive(edit));
      pump.settle(service);
      TEST_CHECK(!service.snapshot().error);
      for (const auto& path : {f.root / "tree", f.root / "tree/child", file}) {
        TEST_CHECK(metadata(path).st_gid == group && metadata(path).st_uid == ::geteuid());
      }
      TEST_CHECK((mode(file) & 0777) == 0754);
      if (::geteuid() != 0 && group != oldGroup) {
        TEST_CHECK((mode(file) & 04000) == 0);
      }
      for (const auto& row : *service.snapshot().results) {
        TEST_CHECK(row.info && row.info->group == group && row.info->mode == mode(*row.location.nativePath()));
      }
      if (group != ::getegid()) {
        break;
      }
    }
    if (::geteuid() != 0) {
      pump.prepare(service, f.root / "tree");
      auto denied = recursiveEdit();
      denied.files.owner = "0";
      const auto before = mode(file);
      TEST_CHECK(service.applyRecursive(denied));
      pump.settle(service);
      TEST_CHECK(service.snapshot().error && !service.snapshot().applied && mode(file) == before);
    }
  }
  void recursiveReplacementDuringCollection(Pump& pump) {
    Fixture f;
    fs::create_directory(f.root / "tree");
    fs::create_directory(f.root / "outside");
    for (unsigned i = 0; i < 1024; ++i) {
      write(f.root / "tree" / std::to_string(i));
    }
    write(f.root / "outside/value");
    const auto before = mode(f.root / "outside/value");
    S service;
    pump.prepare(service, f.root / "tree");
    TEST_CHECK(service.applyRecursive(recursiveEdit()));
    pump.until([&] { return service.completed() > 0 || !service.busy(); });
    fs::rename(f.root / "tree", f.root / "held");
    fs::create_directory_symlink(f.root / "outside", f.root / "tree");
    pump.settle(service);
    TEST_CHECK(mode(f.root / "outside/value") == before);
    for (const auto& row : *service.snapshot().results) {
      TEST_CHECK(!row.location.nativePath()->contains("outside"));
    }
  }
  void recursiveItemLimit(Pump& pump) {
    Fixture f;
    fs::create_directory(f.root / "tree");
    for (unsigned i = 0; i < 16384; ++i) {
      write(f.root / "tree" / std::to_string(i));
    }
    const auto before = mode(f.root / "tree/0");
    S service;
    pump.prepare(service, f.root / "tree");
    TEST_CHECK(service.applyRecursive(recursiveEdit()));
    pump.settle(service);
    TEST_CHECK(service.snapshot().error && !service.snapshot().applied && service.snapshot().finished);
    TEST_CHECK(service.snapshot().message.contains("16384") || service.snapshot().message.contains("file descriptors"));
    for (unsigned i = 0; i < 16384; ++i) {
      TEST_CHECK(mode(f.root / "tree" / std::to_string(i)) == before);
    }
  }
  std::string aclText(const fs::path& path, bool defaults = false) {
    acl_t value = ::acl_get_file(path.c_str(), defaults ? ACL_TYPE_DEFAULT : ACL_TYPE_ACCESS);
    TEST_CHECK(value);
    char* text = ::acl_to_any_text(value, nullptr, '\n', TEXT_NUMERIC_IDS);
    TEST_CHECK(text);
    std::string result(text);
    if (!result.empty() && result.back() != '\n') {
      result += '\n';
    }
    ::acl_free(text);
    ::acl_free(value);
    return result;
  }
  using Tag = files::AclTag;
  std::vector<files::AclEntry> extended(std::string account) {
    return {
        {Tag::Owner, {}, 6},
        {Tag::User, std::move(account), 6},
        {Tag::OwningGroup, {}, 4},
        {Tag::Mask, {}, 4},
        {Tag::Other, {}, 0}
    };
  }
  void applyAcl(Pump& pump, S& service, files::AclEdit edit) {
    TEST_CHECK(service.applyAcl(std::move(edit)));
    pump.settle(service);
    TEST_CHECK(service.snapshot().info && service.snapshot().info->acl);
  }
  bool probeAclSupport() {
    Fixture fixture;
    const auto file = fixture.root / "acl-probe";
    write(file);
    TEST_CHECK(::chmod(file.c_str(), 0600) == 0);
    const auto text = std::format("user::rw-,user:{}:r--,group::---,mask::r--,other::---", ::geteuid());
    acl_t acl = ::acl_from_text(text.c_str());
    TEST_CHECK(acl);
    const int result = ::acl_set_file(file.c_str(), ACL_TYPE_ACCESS, acl);
    const int error = errno;
    ::acl_free(acl);
    if (result == 0) {
      std::println("Extended ACL fixture capability: supported");
      return true;
    }
    TEST_CHECK(error == EOPNOTSUPP || error == ENOSYS || error == EPERM);
    TEST_CHECK(mode(file) == 0600);
    std::println("Extended ACL fixture capability: unavailable ({})", std::strerror(error));
    return false;
  }
  void unavailableAcl(Pump& pump) {
    Fixture fixture;
    const auto file = fixture.root / "file";
    write(file);
    S service;
    pump.prepare(service, file);
    const auto before = mode(file);
    TEST_CHECK(service.snapshot().info->acl);
    applyAcl(pump, service, {.access = extended(std::to_string(::geteuid())), .defaults = {}});
    TEST_CHECK(service.snapshot().error && !service.snapshot().applied && !service.snapshot().message.empty());
    TEST_CHECK(mode(file) == before);
    std::ifstream input(file);
    std::string contents;
    input >> contents;
    TEST_CHECK(contents == "payload");
  }
  void aclEditing(Pump& pump) {
    Fixture f;
    const auto file = f.root / "file";
    write(file);
    S service;
    pump.prepare(service, file);
    if (!service.snapshot().info->acl || !service.snapshot().info->acl->editable) {
      std::println(
          stderr, "ACL fixture unavailable: {}",
          service.snapshot().info->acl ? service.snapshot().info->acl->message : "No ACL metadata"
      );
    }
    TEST_CHECK(
        service.snapshot().info->acl
        && service.snapshot().info->acl->supported
        && service.snapshot().info->acl->editable
    );
    const std::string id = std::to_string(::geteuid() + 20000);
    const auto rules = extended(id);
    applyAcl(pump, service, {.access = rules, .defaults = {}});
    TEST_CHECK(!service.snapshot().error && service.snapshot().applied && mode(file) == 0640);
    TEST_CHECK(aclText(file).contains("user:" + id + ":rw-\n") && aclText(file).contains("mask::r--\n"));
    TEST_CHECK(service.snapshot().info->acl->access == rules);
    applyAcl(pump, service, {.access = rules, .defaults = {}});
    TEST_CHECK(!service.snapshot().error && !service.snapshot().applied);
    // A chmod edits the group-class mask while preserving named rules.
    pump.apply(service, {.mode = 0620, .owner = {}, .group = {}});
    TEST_CHECK(!service.snapshot().error && aclText(file).contains("user:" + id + ":rw-\n"));
    TEST_CHECK(aclText(file).contains("mask::-w-\n"));
    TEST_CHECK(service.snapshot().info->acl->editable);
    applyAcl(pump, service, {.access = rules, .defaults = {}});
    const auto before = aclText(file);
    auto bad = rules;
    bad.push_back({Tag::User, id, 0});
    applyAcl(pump, service, {.access = bad, .defaults = {}});
    TEST_CHECK(service.snapshot().error && !service.snapshot().applied && aclText(file) == before);
    bad = rules;
    bad[1].qualifier = "noctalia-no-such-account-370917";
    applyAcl(pump, service, {.access = bad, .defaults = {}});
    TEST_CHECK(service.snapshot().error && !service.snapshot().applied && aclText(file) == before);
    bad = rules;
    bad[1].permissions = 8;
    applyAcl(pump, service, {.access = bad, .defaults = {}});
    TEST_CHECK(service.snapshot().error && aclText(file) == before);
    bad = rules;
    bad.erase(bad.begin() + 3);
    applyAcl(pump, service, {.access = bad, .defaults = {}});
    TEST_CHECK(service.snapshot().error && aclText(file) == before);
    applyAcl(pump, service, {.access = std::vector<files::AclEntry>{}, .defaults = {}});
    TEST_CHECK(service.snapshot().error && aclText(file) == before);
    applyAcl(pump, service, {.access = rules, .defaults = rules});
    TEST_CHECK(service.snapshot().error && !service.snapshot().applied && aclText(file) == before);
    bad = std::vector<files::AclEntry>(129, {Tag::Other, {}, 0});
    applyAcl(pump, service, {.access = bad, .defaults = {}});
    TEST_CHECK(service.snapshot().error && aclText(file) == before);
    // Account names resolve on the worker and duplicate resolved IDs are refused.
    const auto name = service.snapshot().info->ownerName;
    if (!name.empty()) {
      auto byName = extended(name);
      applyAcl(pump, service, {.access = byName, .defaults = {}});
      TEST_CHECK(!service.snapshot().error && aclText(file).contains("user:" + std::to_string(::geteuid()) + ":rw-\n"));
      byName.push_back({Tag::User, std::to_string(::geteuid()), 0});
      applyAcl(pump, service, {.access = byName, .defaults = {}});
      TEST_CHECK(service.snapshot().error && !service.snapshot().applied);
    }
    // Strip the named entries and mask deliberately, then recover a mode-0000 file.
    applyAcl(
        pump, service,
        {.access = std::vector<files::AclEntry>{{Tag::Owner, {}, 0}, {Tag::OwningGroup, {}, 0}, {Tag::Other, {}, 0}},
         .defaults = {}}
    );
    TEST_CHECK(!service.snapshot().error && mode(file) == 0 && service.snapshot().info->acl->access.size() == 3);
    pump.prepare(service, file);
    applyAcl(pump, service, {.access = rules, .defaults = {}});
    TEST_CHECK(!service.snapshot().error && mode(file) == 0640);
    fs::create_hard_link(file, f.root / "alias");
    pump.prepare(service, file);
    applyAcl(pump, service, {.access = rules, .defaults = {}});
    TEST_CHECK(!service.snapshot().error && aclText(f.root / "alias") == aclText(file));
    std::ifstream contents(file);
    std::string payload;
    contents >> payload;
    TEST_CHECK(payload == "payload");
  }
  void defaultAcls(Pump& pump) {
    Fixture f;
    fs::create_directory(f.root / "folder");
    const auto folder = f.root / "folder";
    write(folder / "existing");
    const auto existing = aclText(folder / "existing");
    const auto rules = extended(std::to_string(::geteuid() + 20000));
    S service;
    pump.prepare(service, folder);
    auto invalid = rules;
    invalid.pop_back();
    const auto access = aclText(folder);
    applyAcl(pump, service, {.access = rules, .defaults = invalid});
    TEST_CHECK(service.snapshot().error && !service.snapshot().applied && aclText(folder) == access);
    applyAcl(pump, service, {.access = {}, .defaults = rules});
    TEST_CHECK(!service.snapshot().error && service.snapshot().applied);
    TEST_CHECK(aclText(folder, true).contains("user:" + std::to_string(::geteuid() + 20000) + ":rw-\n"));
    write(folder / "inherited");
    TEST_CHECK(aclText(folder / "inherited").contains("user:" + std::to_string(::geteuid() + 20000) + ":rw-\n"));
    TEST_CHECK(aclText(folder / "existing") == existing);
    const auto inherited = aclText(folder / "inherited");
    applyAcl(pump, service, {.access = {}, .defaults = std::vector<files::AclEntry>{}});
    TEST_CHECK(!service.snapshot().error && aclText(folder, true).empty());
    write(folder / "plain");
    TEST_CHECK(!aclText(folder / "plain").contains("user:" + std::to_string(::geteuid() + 20000) + ":"));
    TEST_CHECK(aclText(folder / "inherited") == inherited);
    pump.prepare(service, folder);
    const int fd = ::open(folder.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    TEST_CHECK(fd >= 0);
    files::writeAcl(fd, true, rules);
    TEST_CHECK(::close(fd) == 0);
    const auto unchangedAccess = aclText(folder);
    applyAcl(pump, service, {.access = rules, .defaults = {}});
    TEST_CHECK(service.snapshot().error && !service.snapshot().applied && aclText(folder) == unchangedAccess);
    TEST_CHECK(!service.snapshot().info->acl->editable);
  }
  void aclStaleSourcesAndCancellation(Pump& pump) {
    Fixture f;
    const auto file = f.root / "file";
    write(file);
    const auto rules = extended(std::to_string(::geteuid() + 20000));
    S service;
    pump.prepare(service, file);
    applyAcl(pump, service, {.access = rules, .defaults = {}});
    pump.prepare(service, file);
    // Changing a named rule can leave every traditional mode bit unchanged.
    auto other = rules;
    other[1].permissions = 0;
    const int fd = ::open(file.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    TEST_CHECK(fd >= 0);
    files::writeAcl(fd, false, other);
    TEST_CHECK(::close(fd) == 0);
    const auto changed = aclText(file);
    TEST_CHECK(mode(file) == 0640);
    applyAcl(pump, service, {.access = rules, .defaults = {}});
    TEST_CHECK(service.snapshot().error && !service.snapshot().applied && aclText(file) == changed);
    TEST_CHECK(!service.snapshot().info->acl->editable);
    pump.prepare(service, file);
    fs::rename(file, f.root / "held");
    write(file);
    const auto replacement = aclText(file);
    applyAcl(pump, service, {.access = rules, .defaults = {}});
    TEST_CHECK(service.snapshot().error && !service.snapshot().applied && aclText(file) == replacement);
    fs::create_symlink(file, f.root / "link");
    pump.prepare(service, f.root / "link");
    TEST_CHECK(!service.snapshot().info->acl->supported);
    applyAcl(pump, service, {.access = rules, .defaults = {}});
    TEST_CHECK(service.snapshot().error && !service.snapshot().applied && aclText(file) == replacement);
    pump.prepare(service, file);
    TEST_CHECK(service.applyAcl({.access = rules, .defaults = {}}));
    service.cancel();
    pump.settle(service);
    TEST_CHECK(!service.snapshot().applied && aclText(file) == replacement);
  }
} // namespace
int main() {
  supportedSpecialBits = probeSpecialBits();
  Pump pump;
  modesAndAliases(pump);
  ownershipAndValidation(pump);
  linksAndStaleItems(pump);
  cancellationAndLifetime(pump);
  bulkMasksAndOwnership(pump);
  bulkFailuresAndAliases(pump);
  bulkCancellation(pump);
  bulkOwnershipCancellation(pump);
  recursiveModes(pump);
  recursiveFailures(pump);
  recursiveOwnership(pump);
  recursiveReplacementDuringCollection(pump);
  recursiveCancellationAndLimits(pump);
  recursiveItemLimit(pump);
  if (probeAclSupport()) {
    aclEditing(pump);
    defaultAcls(pump);
    aclStaleSourcesAndCancellation(pump);
  } else {
    unavailableAcl(pump);
  }
  std::println("permission service checks passed");
}
