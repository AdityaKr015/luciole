#pragma once

#include "thumbnails/preview_metadata.h"

#include <gio/gio.h>

namespace files {
  // Worker-only source validation and bounded format providers. Missing information is omitted.
  MetadataFields probePreviewMetadata(const DirectoryEntry& entry, ThumbnailOptions options, GCancellable* cancel);
  MetadataFields parseMediaMetadata(std::string_view data);
  MetadataFields parsePdfMetadata(std::string_view data);
} // namespace files
