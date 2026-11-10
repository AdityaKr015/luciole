#pragma once

#include "fs/directory_session.h"
#include "fs/file_error.h"
#include "operations/transfer_service.h"

#include <optional>

namespace luciole {
  enum class RestoreOutcome { Restored, Skipped, Failed, Cancelled, Unconfirmed };
  struct RestoreResult {
    Location source;
    std::optional<Location> destination;
    std::string displayName;
    RestoreOutcome outcome = RestoreOutcome::Cancelled;
    std::optional<FileError> error;
  };
  class RestoreService {
  public:
    RestoreService();
    ~RestoreService();
    RestoreService(const RestoreService&) = delete;
    RestoreService& operator=(const RestoreService&) = delete;
    bool start(std::vector<DirectoryEntry> selection);
    void cancel();
    bool busy() const;
    std::size_t total() const;
    std::size_t completed() const;
    const std::vector<RestoreResult>& results() const;
    std::optional<TransferConflict> conflict() const;
    bool resolve(std::uint64_t id, TransferConflictChoice choice);
    void setChangedCallback(std::function<void()> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace luciole
