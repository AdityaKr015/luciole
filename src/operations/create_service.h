#pragma once

#include "fs/location.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace files {
  enum class CreateKind { FOLDER, EMPTY_FILE };
  enum class CreateStatus { IDLE, PREPARING, READY, CREATING, SUCCEEDED, PARTIAL, ERROR, CANCELLED };
  struct CreateSnapshot {
    CreateStatus status = CreateStatus::IDLE;
    CreateKind kind = CreateKind::FOLDER;
    std::optional<Location> parent;
    std::optional<Location> destination;
    std::string error;
  };

  // One native file or folder creation. Preparation pins the destination while editing.
  class CreateService {
  public:
    CreateService();
    ~CreateService();
    CreateService(const CreateService&) = delete;
    CreateService& operator=(const CreateService&) = delete;
    bool prepare(const Location& parent, CreateKind kind);
    bool create(std::string name);
    void cancel();
    bool busy() const;
    bool canCreate() const;
    const CreateSnapshot& snapshot() const;
    void setChangedCallback(std::function<void()> callback);

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
