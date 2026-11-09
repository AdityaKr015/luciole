#include "operations/archive_sevenzip_provider.h"

#include "fs/gio_interop.h"
#include "operations/archive_provider_process.h"

#include <ranges>
#include <unistd.h>

namespace {
  using namespace files;
  using namespace files::archive_work;
  bool attempt(
      const char* executable, Input& input, int output, const ArchiveEntry& entry, Work& work, std::string_view password
  ) {
    work.check();
    if (const auto problem = archivePasswordProblem(password, ArchiveFormat::SevenZip)) {
      throwFileError(FileErrorKind::InvalidArgument, *problem);
    }
    const std::string include = "-i!" + entry.path;
    const char* const argv[]{executable,        "x",         "-so",  "-t7z", "-bd", "-bb0",   "-bso0",         "-bse0",
                             "-bsp0",           "-sccUTF-8", "-spd", "-ssc", "-r-", "-mmt=1", include.c_str(), "--",
                             "/proc/self/fd/3", nullptr};
    const auto status = runArchiveProvider(input, output, *entry.size, work, password, argv);
    if (status == 2) {
      return false;
    }
    if (status != 0) {
      throwFileError(
          FileErrorKind::Unsupported, "The encrypted 7z provider could not decode this member within its limits."
      );
    }
    const auto result = statFd(output);
    if (result.st_size < 0 || static_cast<std::uint64_t>(result.st_size) != *entry.size) {
      throwFileError(FileErrorKind::InvalidArgument, "The encrypted 7z provider returned an unexpected data size.");
    }
    if (::fsync(output) != 0) {
      systemError("Could not finish decrypted archive data");
    }
    return true;
  }
} // namespace

void files::archive_work::extractEncryptedSevenZip(
    Input& input, int output, const ArchiveEntry& entry, Work& work, const Passwords& passwords, std::uint64_t& total
) {
  const GString executable(g_find_program_in_path("7zz"));
  if (!executable) {
    throwFileError(FileErrorKind::Unavailable, "Encrypted 7z extraction requires the 7zz program.");
  }
  if (!entry.size || *entry.size > work.limits.maxExpandedBytes - total) {
    throwFileError(FileErrorKind::Unsupported, "Encrypted 7z member size is unknown or exceeds the output limit.");
  }
  for (const auto& password : passwords.values | std::views::reverse) {
    if (attempt(executable.get(), input, output, entry, work, password)) {
      total += *entry.size;
      work.progress.bytes.store(total);
      return;
    }
  }
  work.progress.passwordFormat = ArchiveFormat::SevenZip;
  work.progress.passwordNeeded.store(true);
  throwFileError(FileErrorKind::Denied, "Could not unlock the selected 7z member.");
}
