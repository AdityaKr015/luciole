#pragma once

#include "fs/directory_session.h"
#include "fs/file_error.h"
#include "fs/location.h"
#include "operations/template_catalog.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace files {
  enum class CreateKind { Folder, EmptyFile, Document, SymbolicLink, HardLink };
  enum class CreateStatus { Idle, Preparing, Ready, Creating, Succeeded, Partial, Error, Cancelled };
  struct CreateSnapshot {
    CreateStatus status = CreateStatus::Idle;
    CreateKind kind = CreateKind::Folder;
    std::optional<Location> parent;
    std::optional<Location> destination;
    std::optional<FileError> error;
  };

  // One native file or folder creation. Preparation pins the destination while editing.
  class CreateService {
  public:
    CreateService();
    ~CreateService();
    CreateService(const CreateService&) = delete;
    CreateService& operator=(const CreateService&) = delete;
    bool prepare(const Location& parent, CreateKind kind);
    bool prepareTemplate(const Location& parent, DocumentTemplate source);
    bool prepareLink(const DirectoryEntry& source);
    bool createLink(const Location& parent, std::string name, CreateKind kind);
    bool create(std::string name);
    void cancel();
    bool busy() const;
    bool canCreate() const;
    const CreateSnapshot& snapshot() const;
    void setChangedCallback(std::function<void()> callback);

  private:
    bool prepareTarget(const Location& parent, CreateKind kind, std::optional<DocumentTemplate> source);
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace files
