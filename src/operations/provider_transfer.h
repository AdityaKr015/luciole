#pragma once

#include "fs/cancel_token.h"
#include "operations/transfer_service.h"

#include <atomic>
#include <functional>
#include <memory>

namespace luciole {
  namespace transfer {
    struct Work;
  }
  // Worker-owned state for jobs involving a verified GIO remote provider.
  class ProviderTransfer {
  public:
    explicit ProviderTransfer(TransferRequest request);
    ~ProviderTransfer();
    using Choose = std::function<TransferConflictChoice(TransferConflict)>;
    // Local destinations can reuse native publication/history hooks; remote targets and moves ignore them.
    std::vector<TransferResult>
    run(const CancelToken& token, std::atomic<std::uint64_t>& bytes, std::atomic<std::size_t>& completed,
        const Choose& choose, transfer::Work* nativeCopies = nullptr);
    // Counts committed leaves, excluding merges/skips, only when the whole selection completed.
    std::optional<std::size_t> completedCopies() const;
    std::size_t retryable() const;

  private:
    struct State;
    std::unique_ptr<State> m_state;
  };
} // namespace luciole
