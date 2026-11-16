#pragma once

#include "operations/copy_receipt.h"
#include "operations/provider_copy_proof.h"

#include <optional>

namespace luciole {
  struct ProviderCopyStorage;
  class ProviderCopyEffect {
  public:
    static std::shared_ptr<const ProviderCopyEffect> prepare(
        const Location& destination, const Location& staged, provider_history::Budget& budget, const CancelToken& token,
        bool replacement = false, const std::optional<Location>& historyRoot = std::nullopt
    );
    void published(const CancelToken& token, const std::optional<Location>& previous = std::nullopt) const;
    // Worker-only retirement preserves displaced versions when a complete history cannot be captured.
    void preserveInactive() const;

  private:
    friend class ProviderCopyReceipt;
    explicit ProviderCopyEffect(std::shared_ptr<ProviderCopyStorage> storage) : m_storage(std::move(storage)) {}
    std::shared_ptr<ProviderCopyStorage> m_storage;
  };

  // Provider-specific storage/revision checks behind the application's existing Copy history interface.
  class ProviderCopyReceipt final : public CopyReceipt {
  public:
    static std::shared_ptr<const CopyReceipt>
    capture(const std::vector<std::shared_ptr<const ProviderCopyEffect>>& effects, const CancelToken& token);
    CopyReplayResult reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const override;
    std::vector<TransferResult> unconfirmed(const FileError& error) const override;
    std::size_t size() const override;
    static bool cleanupPending();
    static std::vector<FileError> takeCleanupErrors();

  private:
    std::vector<TransferResult> failureResults(const FileError& error) const;
    struct State;
    explicit ProviderCopyReceipt(std::shared_ptr<State> state, std::uint64_t generation);
    std::shared_ptr<State> m_state;
    std::uint64_t m_generation;
  };
} // namespace luciole
