#include "operations/archive_sevenzip_provider.h"

#include "operations/archive_sevenzip_metadata.h"

#include <ranges>
#include <unistd.h>

namespace {
  using namespace files;
  using namespace files::archive_work;
  bool attempt(
      std::uint32_t index, Input& input, int output, const ArchiveEntry& entry, Work& work, std::string_view password
  ) {
    work.check();
    const auto status = runSevenZip(input, output, *entry.size, work, password, index);
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
    Input& input, int output, const ArchiveEntry& entry, Work& work, const Passwords& passwords, std::uint64_t& total,
    std::uint32_t index
) {
  if (!entry.size || *entry.size > work.limits.maxExpandedBytes - total) {
    throwFileError(FileErrorKind::Unsupported, "Encrypted 7z member size is unknown or exceeds the output limit.");
  }
  if (passwords.values.empty() && !entry.encrypted && attempt(index, input, output, entry, work, {})) {
    total += *entry.size;
    work.progress.bytes.store(total);
    return;
  }
  for (const auto& password : passwords.values | std::views::reverse) {
    if (attempt(index, input, output, entry, work, password)) {
      total += *entry.size;
      work.progress.bytes.store(total);
      return;
    }
  }
  work.progress.passwordFormat = ArchiveFormat::SevenZip;
  work.progress.passwordNeeded.store(true);
  throwFileError(FileErrorKind::Denied, "Could not unlock the selected 7z member.");
}
