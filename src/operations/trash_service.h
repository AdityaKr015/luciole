#pragma once

#include "fs/location.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace files {
  enum class TrashStatus { Idle, Preparing, Ready, Trashing, Finished, Error, Cancelled };
  enum class TrashOutcome { Trashed, Failed, Cancelled };
  struct TrashResult {
    Location source;
    TrashOutcome outcome = TrashOutcome::Cancelled;
    std::string error;
  };
  struct TrashSnapshot {
    TrashStatus status = TrashStatus::Idle;
    std::vector<TrashResult> results;
    std::string error;
  };

  // A native browse selection. Preparation pins one parent and its selected entries.
  class TrashService {
  public:
    TrashService();
    ~TrashService();
    TrashService(const TrashService&) = delete;
    TrashService& operator=(const TrashService&) = delete;
    bool prepare(const Location& parent, std::vector<Location> sources);
    bool trash();
    void cancel();
    bool busy() const;
    bool canTrash() const;
    std::size_t completed() const;
    const TrashSnapshot& snapshot() const;
    void setChangedCallback(std::function<void()> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
