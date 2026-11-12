#include "thumbnails/thumbnail_provider.h"

#include "fs/gio_ptr.h"
#include "fs/posix_fd.h"
#include "thumbnails/provider_process.h"
#include "thumbnails/thumbnail_codec.h"
#include "thumbnails/thumbnail_store.h"
#include "thumbnails/thumbnail_worker.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <unistd.h>

namespace {
  std::optional<double> videoDuration(int source, const std::filesystem::path& directory, GCancellable* cancel) {
    const auto path = directory / "duration";
    const luciole::Fd output(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!output.valid()
        || !luciole::runProviderProcess(
            {"ffprobe", "-v", "error", "-protocol_whitelist", "file,pipe", "-show_entries", "format=duration", "-of",
             "default=noprint_wrappers=1:nokey=1", "/proc/self/fd/3"},
            source, cancel, 4096, 1, output.get(), std::chrono::seconds(2)
        )) {
      return std::nullopt;
    }
    const auto bytes = luciole::readThumbnailFile(path, 4096);
    if (!bytes || bytes->empty()) {
      return std::nullopt;
    }
    const auto* begin = reinterpret_cast<const char*>(bytes->data());
    double duration = 0;
    const auto parsed = std::from_chars(begin, begin + bytes->size(), duration);
    return parsed.ec == std::errc{} && std::isfinite(duration) && duration > 0 ? std::optional(duration) : std::nullopt;
  }
  bool darkFrame(const LoadedImageFile& image) {
    std::size_t dark = 0;
    const auto pixels = image.rgba.size() / 4;
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
      dark += image.rgba[i] < 12 && image.rgba[i + 1] < 12 && image.rgba[i + 2] < 12;
    }
    return pixels != 0 && dark * 100 >= pixels * 98;
  }
  std::expected<LoadedImageFile, std::string> videoThumbnail(
      int source, int side, const luciole::ThumbnailOptions& options, GCancellable* cancel,
      const std::filesystem::path& output
  ) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    const auto duration = videoDuration(source, output.parent_path(), cancel);
    const std::vector<double> positions =
        duration && *duration >= 1 ? std::vector{*duration * 0.1, *duration / 3, 0.0} : std::vector{0.0};
    std::optional<LoadedImageFile> dark;
    std::string failure = "no video frame available";
    for (const auto position : positions) {
      if (g_cancellable_is_cancelled(cancel)) {
        return std::unexpected("thumbnail request cancelled");
      }
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (remaining <= std::chrono::milliseconds::zero()) {
        break;
      }
      std::vector<std::string> arguments{
          "ffmpeg",
          "-nostdin",
          "-v",
          "error",
          "-xerror",
          "-threads",
          "1",
          "-max_pixels",
          std::to_string(static_cast<std::uint64_t>(options.maxMegapixels) * 1'000'000U),
          "-filter_threads",
          "1",
          "-protocol_whitelist",
          "file,pipe"
      };
      if (position > 0) {
        arguments.insert(arguments.end(), {"-ss", std::to_string(position)});
      } else {
        arguments.insert(arguments.end(), {"-skip_frame:v", "nokey"});
      }
      arguments.insert(
          arguments.end(),
          {"-i", "/proc/self/fd/3", "-map", "0:V:0", "-frames:v", "1", "-an", "-sn", "-threads", "1", "-vf",
           "scale=" + std::to_string(side) + ':' + std::to_string(side) + ":force_original_aspect_ratio=decrease", "-y",
           output.native()}
      );
      (void)::unlink(output.c_str());
      const auto result =
          luciole::runProviderProcess(std::move(arguments), source, cancel, 8U * 1024U * 1024U, 0, -1, remaining);
      if (!result) {
        failure = result.error();
        continue;
      }
      const auto bytes = luciole::readThumbnailFile(output, 8U * 1024U * 1024U);
      auto image = bytes ? luciole::readThumbnailPng(*bytes, side)
                         : std::expected<luciole::ThumbnailPng, std::string>(std::unexpected("no video frame output"));
      if (!image) {
        failure = image.error();
        continue;
      }
      if (!darkFrame(image->image)) {
        return std::move(image->image);
      }
      if (dark) {
        break;
      }
      dark = std::move(image->image);
    }
    // A genuinely dark video is still a valid preview; do not turn it into a missing thumbnail.
    return dark ? std::expected<LoadedImageFile, std::string>(std::move(*dark)) : std::unexpected(failure);
  }
} // namespace

std::expected<LoadedImageFile, std::string> luciole::renderThumbnailProvider(
    int sourceFd, const std::string& mime, int side, const ThumbnailOptions& options, GCancellable* cancellable
) {
  const bool image = mime.starts_with("image/");
  if (side < 1 || side > (image ? kMaxPreviewImageSide : 1024)) {
    return std::unexpected("invalid preview size");
  }
  const std::size_t outputLimit = image
      ? std::max(std::size_t{8U * 1024U * 1024U}, 12 + static_cast<std::size_t>(side) * side * 4)
      : 8U * 1024U * 1024U;
  const GString temporary(g_dir_make_tmp("luciole-thumbnail-XXXXXX", nullptr));
  if (!temporary) {
    return std::unexpected("cannot prepare thumbnail provider");
  }
  const std::filesystem::path directory(temporary.get());
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{directory};
  const auto output = directory / (mime.starts_with("image/") ? "result.rgba" : "result.png");
  std::expected<void, std::string> success = std::unexpected("no supported thumbnail provider");
  if (mime.starts_with("image/")) {
    // Glycin transfers source pixels through a memfd before scaling. RLIMIT_FSIZE applies there too.
    // The private result remains separately validated against outputLimit and the requested dimensions.
    const auto fileLimit = std::max(
        {static_cast<std::uint64_t>(outputLimit), static_cast<std::uint64_t>(options.maxFileMiB) * 1024U * 1024U,
         static_cast<std::uint64_t>(options.maxMegapixels) * 1'000'000U * 4U}
    );
    success = runProviderProcess(
        {"/proc/self/exe", "--thumbnail-worker", std::to_string(side),
         std::to_string(static_cast<std::uint64_t>(options.maxMegapixels) * 1'000'000U),
         std::to_string(static_cast<std::uint64_t>(options.maxFileMiB) * 1024U * 1024U), mime, output.native()},
        sourceFd, cancellable, fileLimit, 2
    );
  } else if (mime == "application/pdf") {
    success = runProviderProcess(
        {"pdftoppm", "-f", "1", "-singlefile", "-scale-to", std::to_string(side), "-png", "-q", "/proc/self/fd/3",
         (directory / "result").native()},
        sourceFd, cancellable
    );
  } else if (mime.starts_with("video/")) {
    return videoThumbnail(sourceFd, side, options, cancellable, output);
  } else if (mime.starts_with("audio/")) {
    std::vector<std::string> arguments{
        "ffmpeg",
        "-nostdin",
        "-v",
        "error",
        "-xerror",
        "-threads",
        "1",
        "-max_pixels",
        std::to_string(static_cast<std::uint64_t>(options.maxMegapixels) * 1'000'000U),
        "-filter_threads",
        "1",
        "-protocol_whitelist",
        "file,pipe"
    };
    arguments.insert(
        arguments.end(),
        {"-i", "/proc/self/fd/3", "-map", "0:v:0", "-frames:v", "1", "-an", "-sn", "-threads", "1", "-vf",
         std::string("thumbnail=4,scale=")
             + std::to_string(side)
             + ':'
             + std::to_string(side)
             + ":force_original_aspect_ratio=decrease",
         "-y", output.native()}
    );
    success = runProviderProcess(std::move(arguments), sourceFd, cancellable);
  }
  if (!success) {
    if (image && !g_cancellable_is_cancelled(cancellable)) {
      auto report = readThumbnailFile(output, 1024);
      if (report
          && report->size() > 4
          && (*report)[0] == 'N'
          && (*report)[1] == 'F'
          && (*report)[2] == 'E'
          && (*report)[3] == '1') {
        return readThumbnailWorkerResult(std::move(*report), side);
      }
    }
    return std::unexpected(success.error());
  }
  auto encoded = readThumbnailFile(output, outputLimit);
  if (!encoded) {
    return std::unexpected("thumbnail provider exceeded output limits");
  }
  if (mime.starts_with("image/")) {
    return readThumbnailWorkerResult(std::move(*encoded), side);
  }
  auto decoded = readThumbnailPng(*encoded, side);
  if (!decoded) {
    return std::unexpected(decoded.error());
  }
  return std::move(decoded->image);
}
