#pragma once

#include "fs/directory_session.h"

namespace files {
  enum class PermissionPhase { Idle, Loading, Ready, Saving, Error, Cancelled };
  struct PermissionInfo {
    std::uint32_t mode = 0, owner = 0, group = 0;
    std::string ownerName, groupName;
    bool canMode = false, canOwner = false, canGroup = false;
    bool symlink = false, multipleLinks = false, current = true, directory = false;
    std::string reason;
  };
  struct PermissionEdit {
    std::optional<std::uint32_t> mode;
    std::optional<std::string> owner, group;
    std::uint32_t mask = 07777; // Only these mode bits change; ownership can independently clear set-ID bits.
  };
  struct RecursivePermissionEdit {
    PermissionEdit files; // Owner/group changes apply to files and directories.
    std::optional<std::uint32_t> directoryMode;
    std::uint32_t directoryMask = 0;
  };
  enum class PermissionOutcome { Ready, Changed, Unchanged, Failed, Partial, Cancelled, Skipped };
  struct PermissionResult {
    Location location;
    std::string name;
    std::optional<PermissionInfo> info;
    PermissionOutcome outcome = PermissionOutcome::Ready;
    std::string message;
  };
  struct PermissionSnapshot {
    PermissionPhase phase = PermissionPhase::Idle;
    std::optional<PermissionInfo> info;
    std::string message;
    bool error = false, applied = false;
    bool multiple = false, finished = false, cancelRequested = false, recursive = false;
    std::uint32_t mixedMode = 0;
    bool mixedOwner = false, mixedGroup = false;
    std::size_t unknownItems = 0;
    std::shared_ptr<const std::vector<PermissionResult>> results;
    std::string report;
  };
  // Native captured items, up to 4096. Inspection, account lookup and descriptor-based edits run on a worker.
  class PermissionService {
  public:
    PermissionService();
    ~PermissionService();
    PermissionService(const PermissionService&) = delete;
    PermissionService& operator=(const PermissionService&) = delete;
    bool prepare(DirectoryEntry entry);
    bool prepare(std::vector<DirectoryEntry> entries);
    bool prepare(const Location& location); // Capture a Places destination when inspection starts.
    bool apply(PermissionEdit edit);
    bool applyRecursive(RecursivePermissionEdit edit);
    void cancel();
    bool busy() const;
    std::size_t completed() const;
    bool collecting() const;
    std::size_t total() const;
    const PermissionSnapshot& snapshot() const;
    void setChangedCallback(std::function<void()> callback);

  private:
    bool prepareEntry(DirectoryEntry entry, bool captureCurrentLocation);
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
