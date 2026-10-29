#pragma once

#include "fs/location.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace files {
  enum class CreateFolderStatus { IDLE, PREPARING, READY, CREATING, SUCCEEDED, ERROR, CANCELLED };
  struct CreateFolderSnapshot {
    CreateFolderStatus status = CreateFolderStatus::IDLE;
    std::optional<Location> parent;
    std::optional<Location> destination;
    std::string error;
  };

  // One native folder creation. Preparation pins the destination while editing.
  class CreateFolderService {
  public:
    CreateFolderService();
    ~CreateFolderService();
    CreateFolderService(const CreateFolderService&) = delete;
    CreateFolderService& operator=(const CreateFolderService&) = delete;
    bool prepare(const Location& parent);
    bool create(std::string name);
    void cancel();
    bool busy() const;
    bool canCreate() const;
    const CreateFolderSnapshot& snapshot() const;
    void setChangedCallback(std::function<void()> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
