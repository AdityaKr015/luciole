#include "app/operation_hub.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace {
  bool mutating(const OperationJob& job) {
    switch (job.kind()) {
    case JobKind::Rename:
      return job.service<files::RenameService>().snapshot().status == files::RenameStatus::Renaming;
    case JobKind::Create:
      return job.service<files::CreateService>().snapshot().status == files::CreateStatus::Creating;
    case JobKind::Permission:
      return job.service<files::PermissionService>().snapshot().phase == files::PermissionPhase::Saving;
    case JobKind::BatchRename:
      return job.service<files::BatchRenameService>().applying();
    case JobKind::Trash:
      return job.service<files::TrashService>().snapshot().status == files::TrashStatus::Trashing;
    case JobKind::EmptyTrash:
      return job.service<files::EmptyTrashService>().snapshot().status == files::EmptyTrashStatus::Emptying;
    case JobKind::Archive: {
      const auto phase = job.service<files::ArchiveService>().phase();
      return phase == files::ArchivePhase::Compressing
          || phase == files::ArchivePhase::Extracting
          || phase == files::ArchivePhase::Transferring;
    }
    case JobKind::Transfer:
    case JobKind::Restore:
      return job.running();
    }
    std::unreachable();
  }
} // namespace

OperationJob::OperationJob(files::JobId id, Service service) : m_id(id), m_service(std::move(service)) {}

OperationJob::~OperationJob() {
  std::visit([](auto& service) { service->setChangedCallback({}); }, m_service);
}

bool OperationJob::running() const {
  return std::visit([](const auto& service) { return service->busy(); }, m_service);
}

bool OperationJob::pending() const {
  switch (kind()) {
  case JobKind::Trash: {
    const auto& trash = service<files::TrashService>();
    return trash.busy() || trash.canTrash();
  }
  case JobKind::EmptyTrash:
    return service<files::EmptyTrashService>().pending();
  default:
    return running();
  }
}

bool OperationJob::cancellable() const {
  switch (kind()) {
  case JobKind::BatchRename:
    return service<files::BatchRenameService>().applying();
  case JobKind::Transfer:
  case JobKind::Archive:
  case JobKind::Trash:
  case JobKind::Restore:
  case JobKind::EmptyTrash:
    return running();
  default:
    return false;
  }
}

void OperationJob::cancel() {
  std::visit([](auto& service) { service->cancel(); }, m_service);
}

std::optional<files::TransferConflict> OperationJob::conflict() const {
  switch (kind()) {
  case JobKind::Transfer:
    return service<files::TransferService>().conflict();
  case JobKind::Archive:
    return service<files::ArchiveService>().conflict();
  case JobKind::Restore:
    return service<files::RestoreService>().conflict();
  default:
    return std::nullopt;
  }
}

bool OperationJob::resolve(std::uint64_t id, files::TransferConflictChoice choice, bool applyToAll) {
  switch (kind()) {
  case JobKind::Transfer:
    return service<files::TransferService>().resolve(id, choice, applyToAll);
  case JobKind::Archive:
    return service<files::ArchiveService>().resolve(id, choice);
  case JobKind::Restore:
    return service<files::RestoreService>().resolve(id, choice);
  default:
    return false;
  }
}

OperationHub::OperationHub(const files::TabWorkspace& workspace) : m_workspace(workspace) {}

OperationHub::~OperationHub() {
  m_owners.clear();
  for (auto& [id, job] : m_jobs) {
    (void)id;
    std::visit([](auto& service) { service->setChangedCallback({}); }, job->m_service);
    if (auto idle = std::exchange(job->m_idle, {})) {
      idle(*job);
    }
  }
}

void OperationHub::attach(files::TabId page, Owner& owner) { m_owners.insert_or_assign(page, &owner); }

void OperationHub::detach(files::TabId page) { m_owners.erase(page); }

OperationJob* OperationHub::find(files::JobId id) const {
  const auto found = m_jobs.find(id);
  return found == m_jobs.end() ? nullptr : found->second.get();
}

std::shared_ptr<OperationJob> OperationHub::add(files::TabId page, OperationJob::Service service, JobScope scope) {
  const auto id = ++m_next;
  auto job = std::make_shared<OperationJob>(id, std::move(service));
  std::visit(
      [this, raw = job.get()](auto& value) { value->setChangedCallback([this, raw] { changed(*raw); }); },
      job->m_service
  );
  m_routing.add(id, page, m_workspace, scope == JobScope::Follow);
  m_jobs.emplace(id, job);
  return job;
}

void OperationHub::changed(OperationJob& job) {
  if (!job.m_mutationStarted && mutating(job)) {
    job.m_mutationStarted = true;
    job.m_historyGeneration = ++m_historyGeneration;
    if (job.renameReplay() == RenameReplay::None) {
      m_renameReceipt.reset();
      m_renameRedo = false;
    }
  }
  bool finished = false;
  if (job.running()) {
    job.m_running = true;
    job.m_ran = true;
    job.m_resultTaken = false;
  } else if (std::exchange(job.m_running, false)) {
    finished = true;
    if (job.m_mutationStarted && job.kind() == JobKind::Rename) {
      recordRename(job);
    }
    job.m_mutationStarted = false;
    if (auto idle = std::exchange(job.m_idle, {})) {
      idle(job);
    }
  }
  notify(m_routing.owner(job.id()), job);
  if (finished && job.kind() == JobKind::Transfer) {
    for (const auto& [page, owner] : m_owners) {
      (void)page;
      owner->transfersFinished(job.service<files::TransferService>().results());
    }
  }
}

void OperationHub::notify(files::TabId page, OperationJob& job) {
  if (const auto found = m_owners.find(page); found != m_owners.end()) {
    found->second->jobChanged(job);
  }
}

bool OperationHub::retained(const OperationJob& job) const {
  if (job.kind() != JobKind::Transfer || !job.ran()) {
    return false;
  }
  return newest(
             m_routing.owner(job.id()),
             [](const OperationJob& other) { return other.kind() == JobKind::Transfer && other.ran(); }
         )
      == &job;
}

void OperationHub::update(std::optional<files::WindowId> preferred) {
  for (const auto& move : m_routing.update(m_workspace, preferred)) {
    const auto found = m_jobs.find(move.job);
    if (found == m_jobs.end()) {
      continue;
    }
    const auto job = found->second;
    if (move.to) {
      notify(move.to, *job);
    } else if (!m_routing.following(move.job)) {
      job->cancel();
    }
  }
  std::vector<std::shared_ptr<OperationJob>> prepared;
  for (const auto& [id, job] : m_jobs) {
    (void)id;
    if (job->kind() == JobKind::Trash && job->service<files::TrashService>().canTrash()) {
      prepared.push_back(job);
    }
  }
  for (const auto& job : prepared) {
    (void)job->service<files::TrashService>().trash();
  }
  std::erase_if(m_jobs, [this](const auto& item) {
    const auto& job = *item.second;
    if (item.second.use_count() > 1 || job.pending() || retained(job)) {
      return false;
    }
    m_routing.remove(item.first);
    return true;
  });
}

void OperationHub::shutdown() {
  for (auto& [id, job] : m_jobs) {
    (void)id;
    if (job->kind() == JobKind::Archive) {
      job->service<files::ArchiveService>().shutdown();
    } else {
      job->cancel();
    }
  }
}

bool OperationHub::busy() const {
  return std::ranges::any_of(m_jobs, [](const auto& item) { return item.second->running(); });
}

bool OperationHub::renameReplayBusy() const {
  return std::ranges::any_of(m_jobs, [](const auto& item) {
    return item.second->renameReplay() != RenameReplay::None && item.second->running();
  });
}

bool OperationHub::canUndoRename() const { return m_renameReceipt && !m_renameRedo && !busy(); }
bool OperationHub::canRedoRename() const { return m_renameReceipt && m_renameRedo && !busy(); }

bool OperationHub::replayRename(files::TabId page, bool redo) {
  if (!attached(page) || !(redo ? canRedoRename() : canUndoRename())) {
    return false;
  }
  auto job = create<files::RenameService>(page);
  job->m_renameReplay = redo ? RenameReplay::Redo : RenameReplay::Undo;
  return job->service<files::RenameService>().replay(m_renameReceipt);
}

void OperationHub::recordRename(const OperationJob& job) {
  if (job.m_historyGeneration != m_historyGeneration) {
    return;
  }
  const auto& snapshot = job.service<files::RenameService>().snapshot();
  if (snapshot.status == files::RenameStatus::Succeeded) {
    m_renameReceipt = snapshot.receipt;
    m_renameRedo = job.renameReplay() == RenameReplay::Undo;
  } else if (
      snapshot.error
      && (snapshot.error->kind == files::FileErrorKind::Changed
          || snapshot.error->kind == files::FileErrorKind::Missing
          || snapshot.error->kind == files::FileErrorKind::NotDirectory)
  ) {
    m_renameReceipt.reset();
  }
}
