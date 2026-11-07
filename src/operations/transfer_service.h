#pragma once

#include "fs/location.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace files {
  enum class TransferAction { Move, Copy };
  enum class TransferOutcome { Moved, Copied, Skipped, Failed, Cancelled, Partial };
  enum class TransferConflictChoice { Replace, Skip, KeepBoth, Cancel, Merge };
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
    bool canMerge = false;
    bool canApplyToAll = false;
    std::string scopeLabel;
  };
  struct TransferRequest {
    std::vector<Location> sources;
    Location destination;
    TransferAction action = TransferAction::Move;
    bool allowMerge = true; // Archive publication keeps its separate all-or-nothing folder policy.
  };
  struct TransferResult {
    TransferResult(Location location, TransferOutcome value = TransferOutcome::Failed, std::string message = {})
        : source(std::move(location)), outcome(value), error(std::move(message)) {}
    Location source;
    TransferOutcome outcome = TransferOutcome::Failed;
    std::string error;
    std::vector<TransferResult> children;
    bool retryable = false;
    std::optional<Location> destination;
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
    bool resolve(std::uint64_t id, TransferConflictChoice choice, bool applyToAll = false);
    bool retryFailed();
    std::size_t retryable() const;
    const std::string& report() const;
    std::uint64_t jobId() const;
    void setChangedCallback(std::function<void()> callback);

  private:
    bool launch(TransferRequest request, bool retry);
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
