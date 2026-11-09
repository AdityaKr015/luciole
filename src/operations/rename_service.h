#pragma once

#include "fs/file_error.h"
#include "fs/location.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace files {
  enum class RenameStatus { Idle, Preparing, Ready, Renaming, Succeeded, Error, Cancelled };
  struct RenameSnapshot {
    RenameStatus status = RenameStatus::Idle;
    std::optional<Location> source;
    std::optional<Location> destination;
    std::optional<FileError> error;
  };

  // One native rename. Preparation pins identities while the editor is open.
  class RenameService {
  public:
    RenameService();
    ~RenameService();
    RenameService(const RenameService&) = delete;
    RenameService& operator=(const RenameService&) = delete;
    bool prepare(const Location& source);
    bool rename(std::string name);
    void cancel();
    bool busy() const;
    bool canRename() const;
    const RenameSnapshot& snapshot() const;
    void setChangedCallback(std::function<void()> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
