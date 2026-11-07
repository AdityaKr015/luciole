#pragma once

#include "fs/directory_session.h"

namespace files {
  enum class PermissionPhase { Idle, Loading, Ready, Saving, Error, Cancelled };
  struct PermissionInfo {
    std::uint32_t mode = 0, owner = 0, group = 0;
    std::string ownerName, groupName;
    bool canMode = false, canOwner = false, canGroup = false;
    bool symlink = false, multipleLinks = false, current = true;
    std::string reason;
  };
  struct PermissionEdit {
    std::optional<std::uint32_t> mode;
    std::optional<std::string> owner, group;
  };
  struct PermissionSnapshot {
    PermissionPhase phase = PermissionPhase::Idle;
    std::optional<PermissionInfo> info;
    std::string message;
    bool error = false, applied = false;
  };
  // A native item only. All inspection, account lookup and descriptor-based edits run on a worker.
  class PermissionService {
  public:
    PermissionService();
    ~PermissionService();
    PermissionService(const PermissionService&) = delete;
    PermissionService& operator=(const PermissionService&) = delete;
    bool prepare(DirectoryEntry entry);
    bool prepare(const Location& location); // Capture a Places destination when inspection starts.
    bool apply(PermissionEdit edit);
    void cancel();
    bool busy() const;
    const PermissionSnapshot& snapshot() const;
    void setChangedCallback(std::function<void()> callback);

  private:
    bool prepareEntry(DirectoryEntry entry, bool captureCurrentLocation);
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
