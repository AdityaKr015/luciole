#pragma once

#include "fs/cancel_token.h"
#include "operations/transfer_service.h"

#include <atomic>
#include <functional>
#include <memory>

namespace files {
  // Worker-owned state for jobs involving a verified GIO remote provider.
  class ProviderTransfer {
  public:
    explicit ProviderTransfer(TransferRequest request);
    ~ProviderTransfer();
    using Choose = std::function<TransferConflictChoice(TransferConflict)>;
    std::vector<TransferResult>
    run(const CancelToken& token, std::atomic<std::uint64_t>& bytes, std::atomic<std::size_t>& completed,
        const Choose& choose);
    std::size_t retryable() const;

  private:
    struct State;
    std::unique_ptr<State> m_state;
  };
} // namespace files
