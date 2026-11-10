#include "app/operation_hub.h"
#include "event_loop/glib_poll_source.h"
#include "test_check.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <stdexcept>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using files::ActivityState;
  struct Fixture {
    fs::path root;
    Fixture() {
      std::string path = "/tmp/noctalia-activity-XXXXXX";
      TEST_CHECK(::mkdtemp(path.data()));
      root = path;
      fs::create_directory(root / "destination");
    }
    ~Fixture() {
      std::error_code error;
      fs::remove_all(root, error);
    }
    files::Location at(const std::string& name) const { return files::Location::fromPath((root / name).native()); }
    void write(const std::string& name) const { std::ofstream(root / name) << name; }
  };
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::vector<pollfd> fds;
        const auto first = m_source.addPollFds(fds);
        TEST_CHECK(::poll(fds.data(), fds.size(), 1) >= 0);
        m_source.dispatch(fds, first);
      }
    }

  private:
    GlibPollSource m_source;
  };
  struct Owner final : OperationHub::Owner {
    unsigned changes = 0;
    void jobChanged(OperationJob&) override {}
    void transfersFinished(const std::vector<files::TransferResult>&) override {}
    void activityChanged() override { ++changes; }
  };
  struct Workspace {
    files::TabWorkspace model;
    files::WindowId window = model.addWindow();
    files::TabId page = model.addTab(window);
    OperationHub hub{model};
    Owner owner;
    Workspace() { hub.attach(page, owner); }
    ~Workspace() { hub.detach(page); }
  };
  void transferLifecycle(Pump& pump) {
    Fixture fixture;
    fixture.write("success");
    fixture.write("missing");
    fixture.write("destination/missing");
    Workspace workspace;
    auto job = workspace.hub.create<files::TransferService>(workspace.page);
    auto& service = job->service<files::TransferService>();
    TEST_CHECK(workspace.hub.activities().empty());
    TEST_CHECK(service.start(
        {{fixture.at("success"), fixture.at("missing")}, fixture.at("destination"), files::TransferAction::Copy}
    ));
    auto live = workspace.hub.activities();
    TEST_CHECK(live.size() == 1 && !live[0].finished && live[0].canCancel && !live[0].progress);
    pump.until([&] { return service.conflict().has_value(); });
    fs::permissions(fixture.root / "destination", fs::perms::owner_read | fs::perms::owner_exec);
    TEST_CHECK(service.resolve(service.conflict()->id, files::TransferConflictChoice::Replace));
    pump.until([&] { return !service.busy(); });
    fs::permissions(fixture.root / "destination", fs::perms::owner_all);
    auto result = workspace.hub.activities();
    TEST_CHECK(
        result.size() == 1 && result[0].finished && result[0].state == ActivityState::Partial && result[0].canRetry
    );
    TEST_CHECK(result[0].report && result[0].report->contains("missing"));
    TEST_CHECK(!workspace.hub.cancelActivity(job->id()));
    TEST_CHECK(service.retryFailed());
    live = workspace.hub.activities();
    TEST_CHECK(live.size() == 1 && live[0].id == job->id() && !live[0].finished);
    pump.until([&] {
      if (auto conflict = service.conflict()) {
        TEST_CHECK(service.resolve(conflict->id, files::TransferConflictChoice::Replace));
      }
      return !service.busy();
    });
    result = workspace.hub.activities();
    TEST_CHECK(result.size() == 1 && result[0].state == ActivityState::Succeeded && !result[0].canRetry);
    TEST_CHECK(fs::is_regular_file(fixture.root / "destination/missing"));
    TEST_CHECK(workspace.owner.changes >= 4);
    workspace.hub.clearFinishedActivities();
    TEST_CHECK(workspace.hub.activities().empty());
  }
  void conflictCancelAndRehome(Pump& pump) {
    Fixture fixture;
    fixture.write("conflict");
    fixture.write("destination/conflict");
    Workspace workspace;
    const auto other = workspace.model.addTab(workspace.window);
    auto job = workspace.hub.create<files::TransferService>(workspace.page);
    auto& service = job->service<files::TransferService>();
    TEST_CHECK(service.start({{fixture.at("conflict")}, fixture.at("destination"), files::TransferAction::Copy}));
    pump.until([&] { return service.conflict().has_value(); });
    auto live = workspace.hub.activities();
    TEST_CHECK(live.size() == 1 && live[0].state == ActivityState::Waiting && live[0].canReview && live[0].canCancel);
    workspace.hub.clearFinishedActivities();
    TEST_CHECK(workspace.hub.activities().size() == 1 && service.busy());
    TEST_CHECK(workspace.model.close(workspace.page));
    workspace.hub.detach(workspace.page);
    workspace.hub.update(workspace.window);
    TEST_CHECK(workspace.hub.owner(*job) == other && service.conflict());
    TEST_CHECK(workspace.hub.cancelActivity(job->id()));
    pump.until([&] { return !service.busy(); });
    auto result = workspace.hub.activities();
    TEST_CHECK(result.size() == 1 && result[0].state == ActivityState::Cancelled && result[0].finished);
    TEST_CHECK(!workspace.hub.cancelActivity(999999));
    TEST_CHECK(!workspace.hub.activityReport(999999));
  }
  void historyAndInspection(Pump& pump) {
    Fixture fixture;
    Workspace workspace;
    auto inspection = workspace.hub.create<files::PermissionService>(workspace.page, JobScope::Page);
    TEST_CHECK(inspection->service<files::PermissionService>().prepare(fixture.at("destination")));
    pump.until([&] { return !inspection->running(); });
    TEST_CHECK(workspace.hub.activities().empty());
    for (unsigned index = 0; index < 16; ++index) {
      auto job = workspace.hub.create<files::CreateService>(workspace.page, JobScope::Page);
      auto& service = job->service<files::CreateService>();
      TEST_CHECK(service.prepare(fixture.at("destination"), files::CreateKind::EmptyFile));
      pump.until([&] { return !service.busy(); });
      const auto ready = jobActivity(*job);
      TEST_CHECK(ready && !ready->finished && ready->canReview);
      TEST_CHECK(service.create(std::to_string(index)));
      pump.until([&] { return !service.busy(); });
      TEST_CHECK(service.snapshot().status == files::CreateStatus::Succeeded);
      job.reset();
      workspace.hub.update(workspace.window);
    }
    const auto results = workspace.hub.activities();
    TEST_CHECK(results.size() == 12);
    TEST_CHECK(workspace.hub.find(results[0].id) == nullptr);
    TEST_CHECK(workspace.hub.activityReport(results[0].id) && results[0].state == ActivityState::Succeeded);
    TEST_CHECK(g_utf8_validate(results[0].report->data(), static_cast<gssize>(results[0].report->size()), nullptr));
    workspace.hub.clearFinishedActivities();
    TEST_CHECK(workspace.hub.activities().empty());
  }
  void boundedReports(Pump& pump) {
    Fixture fixture;
    Workspace workspace;
    auto job = workspace.hub.create<files::TransferService>(workspace.page);
    auto& service = job->service<files::TransferService>();
    std::vector<files::Location> sources;
    for (unsigned index = 0; index < 600; ++index) {
      std::string name;
      for (unsigned letter = 0; letter < 60; ++letter) {
        name += "文";
      }
      sources.push_back(fixture.at(name + std::to_string(index)));
    }
    TEST_CHECK(service.start({std::move(sources), fixture.at("destination"), files::TransferAction::Copy}));
    pump.until([&] { return !service.busy(); });
    const auto report = workspace.hub.activityReport(job->id());
    TEST_CHECK(report && report->size() <= 64 * 1024 && report->contains("Report truncated."));
    TEST_CHECK(g_utf8_validate(report->data(), static_cast<gssize>(report->size()), nullptr));
    TEST_CHECK(workspace.hub.activities()[0].state == ActivityState::Failed);
  }
  void cancelledArchive(Pump& pump) {
    Fixture fixture;
    fixture.write("input");
    Workspace workspace;
    auto job = workspace.hub.create<files::ArchiveService>(workspace.page);
    auto& archive = job->service<files::ArchiveService>();
    TEST_CHECK(archive.compress({fixture.at("input")}, fixture.at("output.zip"), files::ArchiveFormat::Zip));
    TEST_CHECK(workspace.hub.cancelActivity(job->id()));
    pump.until([&] { return !archive.busy(); });
    const auto results = workspace.hub.activities();
    TEST_CHECK(results.size() == 1 && results[0].finished && results[0].state == ActivityState::Cancelled);
  }
} // namespace

int main() {
  Pump pump;
  transferLifecycle(pump);
  conflictCancelAndRehome(pump);
  historyAndInspection(pump);
  cancelledArchive(pump);
  boundedReports(pump);
}
