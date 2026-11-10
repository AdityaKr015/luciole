#pragma once

#include "fs/file_error.h"
#include "fs/location.h"

#include <optional>
#include <utility>
#include <vector>

namespace files {
  enum class TransferAction { Move, Copy, Link };
  enum class TransferOutcome { Moved, Copied, Skipped, Failed, Cancelled, Partial, Linked };
  struct TransferResult {
    TransferResult(
        Location location, TransferOutcome value = TransferOutcome::Failed, std::optional<FileError> failure = {}
    )
        : source(std::move(location)), outcome(value), error(std::move(failure)) {}
    Location source;
    TransferOutcome outcome = TransferOutcome::Failed;
    std::optional<FileError> error;
    std::vector<TransferResult> children;
    bool retryable = false;
    std::optional<Location> destination;
  };
} // namespace files
