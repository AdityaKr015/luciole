#pragma once

#include "fs/cancel_token.h"
#include "fs/transfer_types.h"

#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace luciole {
  struct MoveIdentity;
  class MoveReceipt;
  struct MoveReplayResult {
    std::vector<TransferResult> results;
    std::shared_ptr<const MoveReceipt> receipt;
    bool retireHistory = false;
  };
  // Proof of one committed, non-replacing native rename between containing folders.
  class MoveEffect {
  public:
    static std::shared_ptr<const MoveEffect>
    capture(const Location& source, const Location& destination, int from, int to, int item);

  private:
    friend class MoveReceipt;
    MoveEffect(
        Location source, Location destination, std::shared_ptr<const MoveIdentity> identity, struct stat revision,
        bool reversed
    );
    int sourceParent() const;
    int destinationParent() const;
    void validate(const struct stat& revision) const;
    void requireVacant() const;
    Location m_source, m_destination;
    std::shared_ptr<const MoveIdentity> m_identity;
    struct stat m_revision;
    bool m_reversed;
  };
  // One application-wide record, with bounded root count and no recursive traversal or data deletion.
  class MoveReceipt {
  public:
    using Revisions = std::map<std::pair<dev_t, ino_t>, struct stat>;
    static std::shared_ptr<const MoveReceipt>
    capture(const std::vector<std::shared_ptr<const MoveEffect>>& effects, const Revisions& revisions);
    MoveReplayResult reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const;
    std::vector<TransferResult> unconfirmed(const FileError& error) const;
    std::size_t size() const { return m_effects.size(); }

  private:
    std::vector<std::shared_ptr<const MoveEffect>> m_effects;
  };
  // Optional proof retention must leave descriptor headroom for the transfer itself.
  std::size_t moveHistoryBudget();
} // namespace luciole
