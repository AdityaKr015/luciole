#pragma once

#include "fs/cancel_token.h"
#include "fs/transfer_types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace luciole {
  struct CopyStorage;
  namespace transfer {
    struct Tree;
  }
  class CopyReceipt;
  struct CopyReplayResult {
    std::vector<TransferResult> results;
    std::shared_ptr<const CopyReceipt> receipt;
    bool retireHistory = false;
  };
  // Worker-only proof captured from a private, unpublished copy, then checked after publication.
  class CopyEffect {
  public:
    static std::shared_ptr<const CopyEffect>
    prepare(const Location& destination, int parent, int stage, std::size_t& remaining, const CancelToken& token);
    std::shared_ptr<const CopyEffect> published() const;

  private:
    friend class CopyReceipt;
    CopyEffect(
        Location destination, std::shared_ptr<CopyStorage> storage, std::shared_ptr<const transfer::Tree> tree,
        bool hidden
    );
    void validate(const CancelToken& token) const;
    void vacant() const;
    std::shared_ptr<const CopyEffect> afterMove(bool hidden) const;
    Location m_destination;
    std::shared_ptr<CopyStorage> m_storage;
    std::shared_ptr<const transfer::Tree> m_tree;
    bool m_hidden;
    std::uint64_t m_generation;
  };
  // One bounded selection. Undo retains unchanged copies privately; Redo publishes the same items.
  class CopyReceipt {
  public:
    static std::shared_ptr<const CopyReceipt> capture(
        const std::vector<std::shared_ptr<const CopyEffect>>& effects, TransferAction action, const CancelToken& token
    );
    CopyReplayResult reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const;
    std::vector<TransferResult> unconfirmed(const FileError& error) const;
    std::size_t size() const { return m_effects.size(); }
    TransferAction action() const { return m_action; }
    static bool cleanupPending();
    static std::vector<FileError> takeCleanupErrors();

  private:
    void retainIncomplete(std::vector<TransferResult>& results) const;
    std::vector<std::shared_ptr<const CopyEffect>> m_effects;
    TransferAction m_action = TransferAction::Copy;
  };
} // namespace luciole
