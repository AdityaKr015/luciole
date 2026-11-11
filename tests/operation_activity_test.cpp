#include "app/operation_hub.h"
#include "event_loop/glib_poll_source.h"
#include "fs/gio_interop.h"
#include "test_check.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <stdexcept>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  using luciole::ActivityState;
  struct Fixture {
    fs::path root;
    Fixture() {
      std::string path = "/tmp/luciole-activity-XXXXXX";
      TEST_CHECK(::mkdtemp(path.data()));
      root = path;
      fs::create_directory(root / "destination");
    }
    ~Fixture() {
      std::error_code error;
      fs::remove_all(root, error);
    }
    luciole::Location at(const std::string& name) const { return luciole::Location::fromPath((root / name).native()); }
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
    void transfersFinished(const std::vector<luciole::TransferResult>&) override {}
    void activityChanged() override { ++changes; }
  };
  struct Workspace {
    luciole::TabWorkspace model;
    luciole::WindowId window = model.addWindow();
    luciole::TabId page = model.addTab(window);
    OperationHub hub{model};
    Owner owner;
    Workspace() { hub.attach(page, owner); }
    ~Workspace() { hub.detach(page); }
  };
  luciole::DirectoryEntry entry(const luciole::Location& location) {
    const luciole::GioPtr<GFileInfo> info(g_file_query_info(
        luciole::gioFile(location).get(), "id::file,id::filesystem,standard::type,standard::is-symlink",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr
    ));
    TEST_CHECK(info);
    return {
        .location = location,
        .name = *location.basename(),
        .displayName = *location.basename(),
        .type = luciole::fileType(g_file_info_get_file_type(info.get())),
        .symlink = g_file_info_get_is_symlink(info.get()) != FALSE,
        .size = {},
        .modifiedSeconds = {},
        .etag = {},
        .parentDisplayName = {},
        .identity = luciole::readFileIdentity(info.get())
    };
  }
  void batchRenameHistory(Pump& pump) {
    Fixture fixture;
    fixture.write("a");
    fixture.write("b");
    Workspace workspace;
    const auto otherWindow = workspace.model.addWindow();
    const auto other = workspace.model.addTab(otherWindow);
    Owner otherOwner;
    workspace.hub.attach(other, otherOwner);
    auto job = workspace.hub.create<luciole::BatchRenameService>(workspace.page);
    auto& service = job->service<luciole::BatchRenameService>();
    TEST_CHECK(service.prepare({entry(fixture.at("a")), entry(fixture.at("b"))}));
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(!workspace.hub.canUndoRename() && !workspace.hub.canRedoRename());
    TEST_CHECK(service.apply());
    pump.until([&] { return !service.busy(); });
    TEST_CHECK(workspace.hub.canUndoRename() && workspace.hub.batchRenameHistory());
    TEST_CHECK(!workspace.hub.canRedoRename());
    job.reset();
    TEST_CHECK(workspace.model.close(workspace.page));
    workspace.hub.detach(workspace.page);
    workspace.hub.update(otherWindow);
    TEST_CHECK(workspace.hub.canUndoRename());
    fixture.write("b");
    TEST_CHECK(workspace.hub.replayRename(other, false));
    TEST_CHECK(workspace.hub.renameReplayBusy() && !workspace.hub.canUndoRename());
    const auto undoActivity = workspace.hub.activities().front();
    TEST_CHECK(undoActivity.title == "Undo batch rename");
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndoRename() && !workspace.hub.canRedoRename());
    TEST_CHECK(fs::exists(fixture.root / "a_001") && !fs::exists(fixture.root / "a"));
    TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Failed);
    TEST_CHECK(fs::remove(fixture.root / "b"));
    TEST_CHECK(workspace.hub.replayRename(other, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(!workspace.hub.canUndoRename() && workspace.hub.canRedoRename());
    TEST_CHECK(fs::exists(fixture.root / "a") && fs::exists(fixture.root / "b"));
    TEST_CHECK(workspace.hub.activities().front().state == ActivityState::Succeeded);
    TEST_CHECK(workspace.hub.replayRename(other, true));
    TEST_CHECK(workspace.hub.activities().front().title == "Redo batch rename");
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndoRename() && !workspace.hub.canRedoRename());
    TEST_CHECK(fs::exists(fixture.root / "a_001") && fs::exists(fixture.root / "b_002"));
    fixture.write("b_002");
    TEST_CHECK(workspace.hub.replayRename(other, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(!workspace.hub.canUndoRename() && !workspace.hub.canRedoRename());
    workspace.hub.detach(other);
  }

  void renameHistoryReplacement(Pump& pump) {
    Fixture fixture;
    fixture.write("a");
    fixture.write("b");
    fixture.write("c");
    fixture.write("d");
    Workspace workspace;
    auto first = workspace.hub.create<luciole::BatchRenameService>(workspace.page);
    auto second = workspace.hub.create<luciole::BatchRenameService>(workspace.page);
    TEST_CHECK(first->service<luciole::BatchRenameService>().prepare({entry(fixture.at("a")), entry(fixture.at("b"))}));
    TEST_CHECK(
        second->service<luciole::BatchRenameService>().prepare({entry(fixture.at("c")), entry(fixture.at("d"))})
    );
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(first->service<luciole::BatchRenameService>().apply());
    TEST_CHECK(second->service<luciole::BatchRenameService>().apply());
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(workspace.hub.canUndoRename());
    TEST_CHECK(workspace.hub.replayRename(workspace.page, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(fs::exists(fixture.root / "a_001") && fs::exists(fixture.root / "b_002"));
    TEST_CHECK(fs::exists(fixture.root / "c") && fs::exists(fixture.root / "d"));
    TEST_CHECK(workspace.hub.canRedoRename());
    auto single = workspace.hub.create<luciole::RenameService>(workspace.page);
    TEST_CHECK(single->service<luciole::RenameService>().prepare(fixture.at("c")));
    pump.until([&] { return !single->running(); });
    TEST_CHECK(workspace.hub.canRedoRename());
    TEST_CHECK(single->service<luciole::RenameService>().rename("single"));
    TEST_CHECK(!workspace.hub.canRedoRename());
    pump.until([&] { return !single->running(); });
    TEST_CHECK(workspace.hub.canUndoRename() && !workspace.hub.batchRenameHistory());
    TEST_CHECK(workspace.hub.replayRename(workspace.page, false));
    pump.until([&] { return !workspace.hub.busy(); });
    TEST_CHECK(fs::exists(fixture.root / "c"));
  }
  void transferLifecycle(Pump& pump) {
    Fixture fixture;
    fixture.write("success");
    fixture.write("missing");
    fixture.write("destination/missing");
    Workspace workspace;
    auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
    auto& service = job->service<luciole::TransferService>();
    TEST_CHECK(workspace.hub.activities().empty());
    TEST_CHECK(service.start(
        {{fixture.at("success"), fixture.at("missing")}, fixture.at("destination"), luciole::TransferAction::Copy}
    ));
    auto live = workspace.hub.activities();
    TEST_CHECK(live.size() == 1 && !live[0].finished && live[0].canCancel && !live[0].progress);
    pump.until([&] { return service.conflict().has_value(); });
    const auto destination = fixture.root / "destination";
    const auto held = fixture.root / "held-destination";
    // A changed destination identity forces failure for both root and ordinary users.
    fs::rename(destination, held);
    fs::create_directory(destination);
    TEST_CHECK(service.resolve(service.conflict()->id, luciole::TransferConflictChoice::Replace));
    pump.until([&] { return !service.busy(); });
    auto result = workspace.hub.activities();
    TEST_CHECK(
        result.size() == 1 && result[0].finished && result[0].state == ActivityState::Partial && result[0].canRetry
    );
    TEST_CHECK(service.results().size() == 2);
    TEST_CHECK(service.results()[0].outcome == luciole::TransferOutcome::Copied);
    TEST_CHECK(service.results()[1].outcome == luciole::TransferOutcome::Failed);
    TEST_CHECK(service.results()[1].error && service.results()[1].error->kind == luciole::FileErrorKind::Changed);
    TEST_CHECK(fs::is_regular_file(held / "success") && fs::is_empty(destination));
    TEST_CHECK(result[0].report && result[0].report->contains("missing"));
    TEST_CHECK(!workspace.hub.cancelActivity(job->id()));
    // Restore the same directory identity so the captured failed item can retry.
    TEST_CHECK(fs::remove(destination));
    fs::rename(held, destination);
    TEST_CHECK(service.retryFailed());
    live = workspace.hub.activities();
    TEST_CHECK(live.size() == 1 && live[0].id == job->id() && !live[0].finished);
    pump.until([&] {
      if (auto conflict = service.conflict()) {
        TEST_CHECK(service.resolve(conflict->id, luciole::TransferConflictChoice::Replace));
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
    auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
    auto& service = job->service<luciole::TransferService>();
    TEST_CHECK(service.start({{fixture.at("conflict")}, fixture.at("destination"), luciole::TransferAction::Copy}));
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
    auto inspection = workspace.hub.create<luciole::PermissionService>(workspace.page, JobScope::Page);
    TEST_CHECK(inspection->service<luciole::PermissionService>().prepare(fixture.at("destination")));
    pump.until([&] { return !inspection->running(); });
    TEST_CHECK(workspace.hub.activities().empty());
    for (unsigned index = 0; index < 16; ++index) {
      auto job = workspace.hub.create<luciole::CreateService>(workspace.page, JobScope::Page);
      auto& service = job->service<luciole::CreateService>();
      TEST_CHECK(service.prepare(fixture.at("destination"), luciole::CreateKind::EmptyFile));
      pump.until([&] { return !service.busy(); });
      const auto ready = jobActivity(*job);
      TEST_CHECK(ready && !ready->finished && ready->canReview);
      TEST_CHECK(service.create(std::to_string(index)));
      pump.until([&] { return !service.busy(); });
      TEST_CHECK(service.snapshot().status == luciole::CreateStatus::Succeeded);
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
    auto job = workspace.hub.create<luciole::TransferService>(workspace.page);
    auto& service = job->service<luciole::TransferService>();
    std::vector<luciole::Location> sources;
    for (unsigned index = 0; index < 600; ++index) {
      std::string name;
      for (unsigned letter = 0; letter < 60; ++letter) {
        name += "文";
      }
      sources.push_back(fixture.at(name + std::to_string(index)));
    }
    TEST_CHECK(service.start({std::move(sources), fixture.at("destination"), luciole::TransferAction::Copy}));
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
    auto job = workspace.hub.create<luciole::ArchiveService>(workspace.page);
    auto& archive = job->service<luciole::ArchiveService>();
    TEST_CHECK(archive.compress({fixture.at("input")}, fixture.at("output.zip"), luciole::ArchiveFormat::Zip));
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
  batchRenameHistory(pump);
  renameHistoryReplacement(pump);
}
