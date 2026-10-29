#pragma once

#include "fs/location.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace files {
  enum class TransferAction { MOVE, COPY };
  enum class TransferOutcome { MOVED, COPIED, SKIPPED, FAILED, CANCELLED, PARTIAL };
  enum class TransferConflictChoice { REPLACE, SKIP, KEEP_BOTH, CANCEL };
  struct TransferConflict {
    std::uint64_t id;
    Location source;
    Location destination;
    std::string name;
    bool sourceDirectory;
    bool destinationDirectory;
    std::optional<std::uint64_t> sourceSize;
    std::optional<std::uint64_t> destinationSize;
    std::optional<std::uint64_t> sourceModified;
    std::optional<std::uint64_t> destinationModified;
  };
  struct TransferRequest {
    std::vector<Location> sources;
    Location destination;
    TransferAction action = TransferAction::MOVE;
  };
  struct TransferResult {
    Location source;
    TransferOutcome outcome = TransferOutcome::FAILED;
    std::string error;
  };

  // One serial native-filesystem job. Workers never access browser models or UI.
  class TransferService {
  public:
    TransferService();
    ~TransferService();
    TransferService(const TransferService&) = delete;
    TransferService& operator=(const TransferService&) = delete;
    bool start(TransferRequest request);
    void cancel();
    bool busy() const;
    std::size_t total() const;
    std::size_t completed() const;
    std::uint64_t bytes() const;
    TransferAction action() const;
    const std::vector<TransferResult>& results() const;
    std::optional<TransferConflict> conflict() const;
    bool resolve(std::uint64_t id, TransferConflictChoice choice);
    void setChangedCallback(std::function<void()> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
