#include "fs/gio_interop.h"

files::FileError files::fileError(const GError& error) {
  using Kind = FileErrorKind;
  auto kind = Kind::Other;
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
    case G_IO_ERROR_IS_DIRECTORY:
      kind = Kind::IsDirectory;
      break;
    case G_IO_ERROR_EXISTS:
      kind = Kind::Exists;
      break;
    case G_IO_ERROR_NOT_EMPTY:
      kind = Kind::NotEmpty;
      break;
    case G_IO_ERROR_FILENAME_TOO_LONG:
      kind = Kind::NameTooLong;
      break;
    case G_IO_ERROR_NO_SPACE:
      kind = Kind::NoSpace;
      break;
    case G_IO_ERROR_READ_ONLY:
      kind = Kind::ReadOnly;
      break;
    case G_IO_ERROR_INVALID_ARGUMENT:
      kind = Kind::InvalidArgument;
      break;
    case G_IO_ERROR_WOULD_RECURSE:
      kind = Kind::WouldRecurse;
      break;
    case G_IO_ERROR_WOULD_MERGE:
      kind = Kind::WouldMerge;
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
    case G_IO_ERROR_CANCELLED:
      kind = Kind::Cancelled;
      break;
    default:
      break;
    }
  }
  return {.kind = kind, .message = error.message};
}

files::FileType files::fileType(GFileType type) {
  switch (type) {
  case G_FILE_TYPE_REGULAR:
    return FileType::Regular;
  case G_FILE_TYPE_DIRECTORY:
    return FileType::Directory;
  case G_FILE_TYPE_SYMBOLIC_LINK:
    return FileType::Symlink;
  case G_FILE_TYPE_SPECIAL:
    return FileType::Special;
  case G_FILE_TYPE_SHORTCUT:
    return FileType::Shortcut;
  case G_FILE_TYPE_MOUNTABLE:
    return FileType::Mountable;
  case G_FILE_TYPE_UNKNOWN:
    break;
  }
  return FileType::Unknown;
}

bool files::readIsSymlink(GFileInfo* info) {
  return g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_STANDARD_TYPE) == G_FILE_TYPE_SYMBOLIC_LINK
      || (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK)
          && g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK));
}
