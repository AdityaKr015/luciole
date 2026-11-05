#pragma once

#include <cstdint>
#include <gio/gio.h>
#include <string>

namespace files {
  enum class FileErrorKind : std::uint8_t { Missing, Denied, NotDirectory, Unavailable, Unsupported, Other };

  struct FileError {
    FileErrorKind kind = FileErrorKind::Other;
    std::string message;
  };

  inline FileError fileError(const GError& error) {
    using Kind = FileErrorKind;
    Kind kind = Kind::Other;
    if (error.domain == G_IO_ERROR) {
      switch (error.code) {
      case G_IO_ERROR_NOT_FOUND:
        kind = Kind::Missing;
        break;
      case G_IO_ERROR_PERMISSION_DENIED:
        kind = Kind::Denied;
        break;
      case G_IO_ERROR_NOT_DIRECTORY:
        kind = Kind::NotDirectory;
        break;
      case G_IO_ERROR_NOT_MOUNTED:
      case G_IO_ERROR_HOST_NOT_FOUND:
      case G_IO_ERROR_HOST_UNREACHABLE:
      case G_IO_ERROR_CONNECTION_CLOSED:
        kind = Kind::Unavailable;
        break;
      case G_IO_ERROR_NOT_SUPPORTED:
        kind = Kind::Unsupported;
        break;
      default:
        break;
      }
    }
    return {.kind = kind, .message = error.message};
  }
} // namespace files
