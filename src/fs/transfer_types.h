#pragma once

#include "fs/location.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace files {
  enum class TransferAction { Move, Copy };
  enum class TransferOutcome { Moved, Copied, Skipped, Failed, Cancelled, Partial };
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
} // namespace files
