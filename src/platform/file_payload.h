#pragma once

#include "operations/transfer_service.h"

#include <expected>
#include <string_view>

namespace files {
  inline constexpr std::string_view kUriListMime = "text/uri-list";
  inline constexpr std::string_view kGnomeFilesMime = "x-special/gnome-copied-files";
  inline constexpr std::string_view kKdeCutMime = "application/x-kde-cutselection";
  inline constexpr std::size_t kFilePayloadLimit = 1024 * 1024;
  inline constexpr std::size_t kFilePayloadCount = 4096;

  struct FilePayload {
    std::vector<Location> locations;
    bool cut = false;
  };

  // Parse the whole immutable offer or reject it. Never silently drop bad sources.
  std::expected<FilePayload, std::string> parseFilePayload(std::string_view mime, std::string_view bytes);
  std::expected<std::string, std::string> encodeFilePayload(const FilePayload& payload, std::string_view mime);
  FilePayload remainingCutFiles(FilePayload payload, const std::vector<TransferResult>& results);
} // namespace files
