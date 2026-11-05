#pragma once

#include <cstdint>
#include <gio/gio.h>
#include <string>

namespace files {
  enum class FileErrorKind : std::uint8_t { MISSING, DENIED, NotDirectory, UNAVAILABLE, UNSUPPORTED, OTHER };

  struct FileError {
    FileErrorKind kind = FileErrorKind::OTHER;
    std::string message;
  };

  inline FileError fileError(const GError& error) {
    using Kind = FileErrorKind;
    Kind kind = Kind::OTHER;
    if (error.domain == G_IO_ERROR) {
      switch (error.code) {
      case G_IO_ERROR_NOT_FOUND:
        kind = Kind::MISSING;
        break;
      case G_IO_ERROR_PERMISSION_DENIED:
        kind = Kind::DENIED;
        break;
      case G_IO_ERROR_NOT_DIRECTORY:
        kind = Kind::NotDirectory;
        break;
      case G_IO_ERROR_NOT_MOUNTED:
      case G_IO_ERROR_HOST_NOT_FOUND:
      case G_IO_ERROR_HOST_UNREACHABLE:
      case G_IO_ERROR_CONNECTION_CLOSED:
        kind = Kind::UNAVAILABLE;
        break;
      case G_IO_ERROR_NOT_SUPPORTED:
        kind = Kind::UNSUPPORTED;
        break;
      default:
        break;
      }
    }
    return {.kind = kind, .message = error.message};
  }
} // namespace files
