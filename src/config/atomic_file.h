#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace files {
  enum class AtomicWriteState : unsigned char {
    UNCHANGED,
    NOT_COMMITTED,
    COMMITTED, // Visible replacement, but directory sync failed. Do not report rollback.
    DURABLE,
  };

  struct AtomicWriteResult {
    AtomicWriteState state = AtomicWriteState::NOT_COMMITTED;
    std::string error;
  };

  // nullopt leaves the file unchanged; an error aborts before replacement.
  using AtomicTransform =
      std::function<std::expected<std::optional<std::string>, std::string>(const std::optional<std::string>&)>;

  // For private, application-owned local files. Serializes cooperating writers
  // with a persistent advisory lock. Call outside Render, on an I/O worker.
  [[nodiscard]] AtomicWriteResult
  updateTextFileAtomic(const std::filesystem::path& path, const AtomicTransform& transform);
} // namespace files
