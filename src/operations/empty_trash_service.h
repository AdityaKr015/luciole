#pragma once

#include "fs/location.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace files {
  enum class EmptyTrashStatus { IDLE, PREPARING, READY, EMPTYING, FINISHED, ERROR, CANCELLED };
  enum class EmptyTrashOutcome { DELETED, SKIPPED, FAILED, CANCELLED, UNCONFIRMED };
  struct EmptyTrashResult {
    Location location;
    std::string displayName;
    EmptyTrashOutcome outcome = EmptyTrashOutcome::CANCELLED;
    std::string error;
  };
  struct EmptyTrashSnapshot {
    EmptyTrashStatus status = EmptyTrashStatus::IDLE;
    std::vector<EmptyTrashResult> results;
    std::string error;
    std::optional<std::uint64_t> size = std::nullopt;
    bool sizeIncomplete = false;
  };

  // Only the standard Trash root is accepted; preparation never removes anything.
  class EmptyTrashService {
  public:
    EmptyTrashService();
    ~EmptyTrashService();
    EmptyTrashService(const EmptyTrashService&) = delete;
    EmptyTrashService& operator=(const EmptyTrashService&) = delete;
    bool prepare();
    bool empty();
    void cancel();
    bool busy() const;
    bool pending() const;
    bool canEmpty() const;
    std::size_t completed() const;
    const EmptyTrashSnapshot& snapshot() const;
    void setChangedCallback(std::function<void()> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
