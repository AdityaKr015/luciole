#pragma once

#include "fs/directory_session.h"

namespace files {
  enum class BatchRenameMode { Numbering, Replace };
  struct BatchRenameOptions {
    BatchRenameMode mode = BatchRenameMode::Numbering;
    std::string pattern = "{name}_{number}", find, replacement;
    int start = 1, digits = 3;
    bool keepExtensions = true;
  };
  enum class BatchRenamePhase { Idle, Preparing, Previewing, Ready, Applying, Finished, Error, Cancelled };
  enum class BatchRenameOutcome { Renamed, Unchanged, Failed, Cancelled, NeedsAttention, Unconfirmed };
  struct BatchRenameRow {
    std::string before, after, name, error;
    bool changed = false;
  };
  struct BatchRenameResult {
    Location source;
    std::optional<Location> actual;
    BatchRenameOutcome outcome;
    std::string error;
  };
  struct BatchRenameSnapshot {
    BatchRenamePhase phase = BatchRenamePhase::Idle;
    std::shared_ptr<const std::vector<BatchRenameRow>> rows;
    std::shared_ptr<const std::vector<BatchRenameResult>> results;
    std::optional<Location> parent;
    std::string error, report;
    bool canApply = false;
  };
  // One native containing folder, up to 4096 captured entries, in their displayed selection order.
  class BatchRenameService {
  public:
    BatchRenameService();
    ~BatchRenameService();
    BatchRenameService(const BatchRenameService&) = delete;
    BatchRenameService& operator=(const BatchRenameService&) = delete;
    bool prepare(std::vector<DirectoryEntry> entries);
    bool preview(BatchRenameOptions options);
    bool apply();
    void cancel();
    bool busy() const;
    bool applying() const;
    std::size_t completed() const;
    const BatchRenameSnapshot& snapshot() const;
    void setChangedCallback(std::function<void()> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
  std::string
  batchRenameName(const std::string& name, bool directory, std::size_t index, const BatchRenameOptions& options);
} // namespace files
