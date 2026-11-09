#include "operations/archive_sevenzip_metadata.h"

#include "fs/gio_interop.h"
#include "operations/archive_provider_process.h"

#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <sys/mman.h>
#include <unistd.h>

std::string files::archive_work::sevenZipWorker() {
  // Explicit override supports uninstalled consumers and isolated provider fixtures.
  if (const auto* override = g_getenv("NOCTALIA_FILES_ARCHIVE_WORKER")) {
    if (!std::filesystem::path(override).is_absolute()) {
      throwFileError(FileErrorKind::Unavailable, "The archive worker path must be absolute.");
    }
    return override;
  }
  const GString executable(g_file_read_link("/proc/self/exe", nullptr));
  if (!executable) {
    throwFileError(FileErrorKind::Unavailable, "Could not locate the archive worker.");
  }
  return (std::filesystem::path(executable.get()).parent_path() / "noctalia-files-archive-worker").string();
}
int files::archive_work::runSevenZip(
    Input& input, int output, std::uint64_t maximum, Work& work, std::string_view password,
    std::optional<std::uint32_t> index
) {
  if (const auto problem = archivePasswordProblem(password, ArchiveFormat::SevenZip)) {
    throwFileError(FileErrorKind::InvalidArgument, *problem);
  }
  const auto executable = sevenZipWorker();
  const auto member = std::to_string(index.value_or(0));
  const auto metadata = std::to_string(std::min<std::size_t>(work.limits.maxMetadataBytes, 16U * 1024U * 1024U));
  const auto entries = std::to_string(std::min<std::size_t>(work.limits.maxEntries, 50000));
  const auto source = std::to_string(work.limits.maxArchiveBytes), bytes = std::to_string(maximum);
  const char* const argv[]{executable.c_str(), index ? "extract" : "catalog",
                           member.c_str(),     metadata.c_str(),
                           entries.c_str(),    source.c_str(),
                           bytes.c_str(),      nullptr};
  const auto status = runArchiveProvider(input, output, maximum, work, password, argv);
  if (status == 69) {
    throwFileError(
        FileErrorKind::Unavailable, "Encrypted 7z requires a compatible system 7-Zip library (23 through 26)."
    );
  }
  return status;
}
std::vector<files::archive_work::SevenZipMember>
files::archive_work::sevenZipCatalog(Input& input, Work& work, const Passwords& passwords) {
  const auto maximum = std::min<std::size_t>(work.limits.maxMetadataBytes, 16U * 1024U * 1024U);
  if (maximum < 32) {
    throwFileError(FileErrorKind::Unsupported, "The 7z catalog exceeds its metadata limit.");
  }
  const auto output = opened(::memfd_create("noctalia-files-7z-catalog", MFD_CLOEXEC));
  for (std::size_t attempt = 0; attempt < std::max<std::size_t>(1, passwords.values.size()); ++attempt) {
    const std::string_view password =
        passwords.values.empty() ? std::string_view{} : passwords.values[passwords.values.size() - attempt - 1];
    const auto status = runSevenZip(input, output.get(), maximum, work, password, std::nullopt);
    if (status == 2) {
      continue;
    }
    if (status != 0) {
      throwFileError(
          FileErrorKind::Unsupported, "The encrypted 7z catalog contains unsupported metadata or exceeds its limits."
      );
    }
    const auto size = statFd(output.get()).st_size;
    if (size <= 0 || static_cast<std::uint64_t>(size) > maximum) {
      throwFileError(FileErrorKind::InvalidArgument, "Invalid 7z catalog size.");
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      work.check();
      const auto count =
          ::pread(output.get(), bytes.data() + offset, bytes.size() - offset, static_cast<off_t>(offset));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        systemError("Could not read the 7z catalog");
      }
      offset += static_cast<std::size_t>(count);
    }
    try {
      const auto document = nlohmann::json::parse(bytes);
      if (document.at("version") != 1
          || !document.at("entries").is_array()
          || document.at("entries").size() > work.limits.maxEntries) {
        throw std::runtime_error("Invalid catalog version or count");
      }
      std::vector<SevenZipMember> result;
      for (const auto& item : document.at("entries")) {
        if (!item.at("index").is_number_unsigned()
            || item.at("index").get<std::uint64_t>() != result.size()
            || !item.at("size").is_number_unsigned()
            || item.at("size").get<std::uint64_t>() > INT64_MAX
            || !item.at("mode").is_number_unsigned()
            || item.at("mode").get<std::uint64_t>() > 0777U) {
          throw std::runtime_error("Invalid catalog fields");
        }
        result.push_back(
            {item.at("index").get<std::uint32_t>(), item.at("path").get<std::string>(),
             item.at("size").get<std::uint64_t>(), item.at("mode").get<unsigned>(), item.at("directory").get<bool>(),
             item.at("encrypted").get<bool>()}
        );
        if (result.back().path.contains('\0')) {
          throw std::runtime_error("Invalid catalog name");
        }
      }
      return result;
    } catch (const std::exception&) {
      throwFileError(FileErrorKind::InvalidArgument, "The 7z provider returned an invalid catalog.");
    }
  }
  work.progress.passwordFormat = ArchiveFormat::SevenZip;
  work.progress.passwordNeeded.store(true);
  throwFileError(FileErrorKind::Denied, "Could not unlock the 7z header.");
}
