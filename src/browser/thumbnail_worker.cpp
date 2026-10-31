#include "browser/thumbnail_worker.h"

#include "browser/thumbnail_codec.h"
#include "fs/gio_ptr.h"

#include <array>
#include <charconv>
#include <fcntl.h>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>

int files::runThumbnailWorker(std::span<const std::string_view> arguments) {
  if (arguments.size() != 4)
    return 2;
  const auto number = [](std::string_view input) -> std::optional<std::uint64_t> {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size() ? std::optional(value) : std::nullopt;
  };
  const auto side = number(arguments[0]), pixels = number(arguments[1]), maximum = number(arguments[2]);
  if (!side
      || *side < 1
      || *side > 1024
      || !pixels
      || *pixels < 1
      || *pixels > 64'000'000
      || !maximum
      || *maximum > 1024U * 1024U * 1024U)
    return 2;
  struct stat info{};
  if (::fstat(3, &info) < 0
      || !S_ISREG(info.st_mode)
      || info.st_size <= 0
      || static_cast<std::uint64_t>(info.st_size) > *maximum)
    return 2;
  try {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto count = ::read(3, bytes.data() + offset, bytes.size() - offset);
      if (count <= 0)
        return 1;
      offset += static_cast<std::size_t>(count);
    }
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    auto image = decodeThumbnail(bytes, static_cast<int>(*side), *pixels, cancel.get());
    if (!image)
      return 1;
    // Private parent/child transport avoids PNG compression followed immediately by decoding.
    std::array<std::uint8_t, 12> header{'N', 'F', 'R', '1'};
    for (int i = 0; i < 4; ++i) {
      header[4 + i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(image->width) >> (24 - i * 8));
      header[8 + i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(image->height) >> (24 - i * 8));
    }
    const int output =
        ::open(std::string(arguments[3]).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output < 0)
      return 1;
    for (const std::span<const std::uint8_t> data :
         {std::span<const std::uint8_t>(header), std::span<const std::uint8_t>(image->rgba)}) {
      offset = 0;
      while (offset < data.size()) {
        const auto count = ::write(output, data.data() + offset, data.size() - offset);
        if (count <= 0) {
          ::close(output);
          return 1;
        }
        offset += static_cast<std::size_t>(count);
      }
    }
    return ::close(output) == 0 ? 0 : 1;
  } catch (const std::exception&) {
    return 1;
  }
}

std::expected<LoadedImageFile, std::string>
files::readThumbnailWorkerResult(std::vector<std::uint8_t> bytes, int maxSide) {
  if (bytes.size() < 12
      || maxSide < 1
      || maxSide > 1024
      || bytes[0] != 'N'
      || bytes[1] != 'F'
      || bytes[2] != 'R'
      || bytes[3] != '1')
    return std::unexpected("invalid thumbnail worker result");
  const auto dimension = [&](std::size_t offset) {
    std::uint32_t result = 0;
    for (std::size_t i = offset; i < offset + 4; ++i)
      result = (result << 8) | bytes[i];
    return result;
  };
  const auto width = dimension(4), height = dimension(8);
  if (!width
      || !height
      || width > static_cast<unsigned>(maxSide)
      || height > static_cast<unsigned>(maxSide)
      || bytes.size() != 12 + static_cast<std::size_t>(width) * height * 4)
    return std::unexpected("oversized or truncated thumbnail worker result");
  bytes.erase(bytes.begin(), bytes.begin() + 12);
  return LoadedImageFile{
      .rgba = std::move(bytes), .width = static_cast<int>(width), .height = static_cast<int>(height)
  };
}
