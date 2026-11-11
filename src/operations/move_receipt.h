#pragma once

#include "fs/cancel_token.h"
#include "fs/posix_fd.h"
#include "fs/transfer_types.h"

#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace luciole {
  struct MoveIdentity;
  class CopyEffect;
  using MoveParents = std::map<std::string, int>;
  class MoveReceipt;
  struct MoveReplayResult {
    std::vector<TransferResult> results;
    std::shared_ptr<const MoveReceipt> receipt;
    bool retireHistory = false;
  };
  // One removed merge container, retained empty until Undo restores its original inode.
  class MoveFolder {
  public:
    static std::shared_ptr<const MoveFolder>
    retain(const Location& source, int from, const Location& folder, int target, const CancelToken& token);
    std::vector<Location> retainForRecovery() const;

  private:
    friend class MoveReceipt;
    MoveFolder(Location sourceValue, Location folderValue)
        : m_source(std::move(sourceValue)), m_retainingFolder(std::move(folderValue)) {}
    Location m_source, m_retainingFolder;
    std::shared_ptr<const CopyEffect> m_item;
    std::shared_ptr<const Fd> m_from, m_target;
  };
  // Proof of one committed native rename, optionally paired with its displaced destination.
  class MoveEffect {
  public:
    static std::shared_ptr<const MoveEffect>
    capture(const Location& source, const Location& destination, int from, int to, int item);
    static std::shared_ptr<const MoveEffect> prepareReplacement(
        const Location& source, const Location& destination, int from, int to, std::size_t& remaining,
        const CancelToken& token
    );
    std::shared_ptr<const MoveEffect> publishedReplacement() const;
    std::vector<Location> retainForRecovery() const;

  private:
    friend class MoveReceipt;
    MoveEffect(
        Location source, Location destination, std::shared_ptr<const MoveIdentity> identity, struct stat revision,
        bool reversed, std::shared_ptr<const CopyEffect> previous = {}
    );
    int sourceParent() const;
    int destinationParent() const;
    void validate(const struct stat& revision, const MoveParents& parents = {}) const;
    void requireVacant(const MoveParents& parents = {}) const;
    Location m_source, m_destination;
    std::shared_ptr<const MoveIdentity> m_identity;
    struct stat m_revision;
    bool m_reversed;
    std::shared_ptr<const CopyEffect> m_previous;
  };
  // One bounded application-wide native move record, including retained replacement/merge data.
  class MoveReceipt {
  public:
    using Revisions = std::map<std::pair<dev_t, ino_t>, struct stat>;
    static std::shared_ptr<const MoveReceipt> capture(
        const std::vector<std::shared_ptr<const MoveEffect>>& effects, const Revisions& revisions,
        const std::vector<std::shared_ptr<const MoveFolder>>& folders = {}, bool undone = false
    );
    MoveReplayResult reverse(const CancelToken& token, std::atomic<std::size_t>& completed) const;
    std::vector<TransferResult> unconfirmed(const FileError& error) const;
    std::size_t size() const { return m_effects.size() + m_folders.size(); }

  private:
    MoveParents parents() const;
    void retainIncomplete(std::vector<TransferResult>& results) const;
    std::vector<std::shared_ptr<const MoveEffect>> m_effects;
    std::vector<std::shared_ptr<const MoveFolder>> m_folders;
    bool m_undone = false;
    std::shared_ptr<std::mutex> m_mutex = std::make_shared<std::mutex>();
  };
  // Optional proof retention must leave descriptor headroom for the transfer itself.
  std::size_t transferHistoryBudget(std::size_t descriptorsPerRoot);
} // namespace luciole
