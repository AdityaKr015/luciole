#include "app/operation_hub.h"

#include <algorithm>
#include <utility>
#include <vector>

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
  bool finished = false;
  if (job.running()) {
    job.m_running = true;
    job.m_ran = true;
    job.m_resultTaken = false;
  } else if (std::exchange(job.m_running, false)) {
    finished = true;
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
