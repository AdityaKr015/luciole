#include "thumbnails/thumbnail_provider.h"

#include "fs/gio_ptr.h"
#include "thumbnails/provider_process.h"
#include "thumbnails/thumbnail_codec.h"
#include "thumbnails/thumbnail_store.h"
#include "thumbnails/thumbnail_worker.h"

#include <algorithm>
#include <filesystem>

std::expected<LoadedImageFile, std::string> files::renderThumbnailProvider(
    int sourceFd, const std::string& mime, int side, const ThumbnailOptions& options, GCancellable* cancellable
) {
  const bool image = mime.starts_with("image/");
  if (side < 1 || side > (image ? kMaxPreviewImageSide : 1024)) {
    return std::unexpected("invalid preview size");
  }
  const std::size_t outputLimit = image
      ? std::max(std::size_t{8U * 1024U * 1024U}, 12 + static_cast<std::size_t>(side) * side * 4)
      : 8U * 1024U * 1024U;
  const GString temporary(g_dir_make_tmp("noctalia-thumbnail-XXXXXX", nullptr));
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
  } else if (mime.starts_with("video/") || mime.starts_with("audio/")) {
    const bool video = mime.starts_with("video/");
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
    // A tiny still needs one keyframe, not a full-resolution multi-frame histogram.
    // Starting at the beginning also works for very short and single-GOP clips.
    if (video) {
      arguments.insert(arguments.end(), {"-skip_frame:v", "nokey"});
    }
    arguments.insert(
        arguments.end(),
        {"-i", "/proc/self/fd/3", "-map", video ? "0:V:0" : "0:v:0", "-frames:v", "1", "-an", "-sn", "-vf",
         std::string(video ? "scale=" : "thumbnail=4,scale=")
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
