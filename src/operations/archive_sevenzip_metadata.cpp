#include "operations/archive_sevenzip_metadata.h"

#include "fs/gio_interop.h"
#include "noctalia_files_sevenzip_metadata.h"
#include "operations/archive_provider_process.h"

#include <algorithm>
#include <ranges>
#include <sys/mman.h>
#include <unistd.h>

std::vector<char> files::archive_work::decodeSevenZipHeader(Input& input, Work& work, const Passwords& passwords) {
  const GString executable(g_find_program_in_path("python3"));
  if (!executable) {
    throwFileError(FileErrorKind::Unavailable, "Encrypted 7z headers require Python 3 with py7zr.");
  }
  const auto maximum = std::min<std::size_t>(work.limits.maxMetadataBytes, 16U * 1024U * 1024U);
  const auto limit = std::to_string(maximum);
  const char* const argv[]{executable.get(), "-I", "-c", sevenZipMetadataProgram, limit.c_str(), nullptr};
  const auto output = opened(::memfd_create("noctalia-files-7z-header", MFD_CLOEXEC));
  // A blank attempt verifies installed provider support before asking for a password.
  for (std::size_t index = 0; index < std::max<std::size_t>(1, passwords.values.size()); ++index) {
    const std::string_view password =
        passwords.values.empty() ? std::string_view{} : passwords.values[passwords.values.size() - index - 1];
    if (const auto problem = archivePasswordProblem(password, ArchiveFormat::SevenZip)) {
      throwFileError(FileErrorKind::InvalidArgument, *problem);
    }
    const auto status = runArchiveProvider(input, output.get(), maximum, work, password, argv);
    if (status == 69) {
      throwFileError(FileErrorKind::Unavailable, "Encrypted 7z headers require Python 3 with py7zr.");
    }
    if (status == 2 || status == 3) {
      continue;
    }
    if (status != 0) {
      throwFileError(
          FileErrorKind::Unsupported,
          "The encrypted 7z metadata provider could not decode this header within its limits."
      );
    }
    const auto size = statFd(output.get()).st_size;
    if (size <= 0 || static_cast<std::uint64_t>(size) > maximum) {
      throwFileError(FileErrorKind::InvalidArgument, "The 7z metadata provider returned an invalid header size.");
    }
    std::vector<char> result(static_cast<std::size_t>(size));
    std::size_t offset = 0;
    while (offset < result.size()) {
      work.check();
      const auto count =
          ::pread(output.get(), result.data() + offset, result.size() - offset, static_cast<off_t>(offset));
      if (count <= 0) {
        systemError("Could not read the decoded 7z header");
      }
      offset += static_cast<std::size_t>(count);
    }
    return result;
  }
  work.progress.passwordFormat = ArchiveFormat::SevenZip;
  work.progress.passwordNeeded.store(true);
  throwFileError(FileErrorKind::Denied, "Could not unlock the 7z header.");
}
