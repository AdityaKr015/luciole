#include "browser/thumbnail_provider.h"

#include "browser/thumbnail_codec.h"
#include "browser/thumbnail_store.h"
#include "browser/thumbnail_worker.h"
#include "fs/gio_ptr.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>

namespace {
  struct ChildLimits {
    pid_t parent;
    rlim_t outputBytes;
  };
  void childSetup(gpointer data) {
    const auto& limits = *static_cast<const ChildLimits*>(data);
    (void)::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() != limits.parent)
      ::_exit(1);
    (void)::setsid();
    errno = 0;
    if (::nice(10) == -1 && errno != 0)
      ::_exit(1);
    const rlimit cpu{5, 5}, memory{768U * 1024U * 1024U, 768U * 1024U * 1024U},
        file{limits.outputBytes, limits.outputBytes};
    if (::setrlimit(RLIMIT_CPU, &cpu) < 0)
      ::_exit(1);
#ifndef __SANITIZE_ADDRESS__
    // ASan reserves a large virtual shadow region before the helper enters main.
    if (::setrlimit(RLIMIT_AS, &memory) < 0)
      ::_exit(1);
#else
    (void)memory;
#endif
    if (::setrlimit(RLIMIT_FSIZE, &file) < 0)
      ::_exit(1);
  }
  bool
  run(std::vector<std::string> args, int input, GCancellable* cancellable, rlim_t outputBytes = 8U * 1024U * 1024U) {
    if (g_cancellable_is_cancelled(cancellable))
      return false;
    const files::GString executable(g_find_program_in_path(args.front().c_str()));
    if (!executable)
      return false;
    args.front() = executable.get();
    const files::GioPtr<GSubprocessLauncher> launcher(g_subprocess_launcher_new(
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE)
    ));
    ChildLimits limits{::getpid(), outputBytes};
    g_subprocess_launcher_set_child_setup(launcher.get(), childSetup, &limits, nullptr);
    const int source = ::fcntl(input, F_DUPFD_CLOEXEC, 4);
    if (source < 0)
      return false;
    g_subprocess_launcher_take_fd(launcher.get(), source, 3);
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& argument : args)
      argv.push_back(argument.c_str());
    argv.push_back(nullptr);
    const files::GioPtr<GSubprocess> process(g_subprocess_launcher_spawnv(launcher.get(), argv.data(), nullptr));
    if (!process)
      return false;
    // Waiting occurs only on a thumbnail worker. The child is also CPU/address-space/output bounded.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    struct Wait {
      std::mutex mutex;
      std::condition_variable condition;
      bool finished = false, cancelled = false;
    } wait;
    struct Disconnect {
      GCancellable* source;
      gulong id = 0;
      ~Disconnect() {
        if (source)
          g_cancellable_disconnect(source, id);
      }
    } cancellation{cancellable};
    if (cancellable)
      cancellation.id = g_cancellable_connect(
          cancellable, G_CALLBACK(+[](GCancellable*, gpointer data) {
            auto& wait = *static_cast<Wait*>(data);
            {
              std::scoped_lock lock(wait.mutex);
              wait.cancelled = true;
            }
            wait.condition.notify_one();
          }),
          &wait, nullptr
      );
    std::thread watchdog([&] {
      std::unique_lock lock(wait.mutex);
      wait.condition.wait_until(lock, deadline, [&] { return wait.finished || wait.cancelled; });
      if (wait.finished)
        return;
      lock.unlock();
      g_subprocess_force_exit(process.get());
    });
    const bool done = g_subprocess_wait_check(process.get(), nullptr, nullptr);
    {
      std::scoped_lock lock(wait.mutex);
      wait.finished = true;
    }
    wait.condition.notify_one();
    watchdog.join();
    return done && !g_cancellable_is_cancelled(cancellable);
  }
} // namespace

std::expected<LoadedImageFile, std::string> files::renderThumbnailProvider(
    int sourceFd, const std::string& mime, int side, const ThumbnailOptions& options, GCancellable* cancellable
) {
  const bool image = mime.starts_with("image/");
  if (side < 1 || side > (image ? kMaxPreviewImageSide : 1024))
    return std::unexpected("invalid preview size");
  const std::size_t outputLimit = image
      ? std::max(std::size_t{8U * 1024U * 1024U}, 12 + static_cast<std::size_t>(side) * side * 4)
      : 8U * 1024U * 1024U;
  const GString temporary(g_dir_make_tmp("noctalia-thumbnail-XXXXXX", nullptr));
  if (!temporary)
    return std::unexpected("cannot prepare thumbnail provider");
  const std::filesystem::path directory(temporary.get());
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{directory};
  const auto output = directory / (mime.starts_with("image/") ? "result.rgba" : "result.png");
  bool success = false;
  if (mime.starts_with("image/")) {
    success =
        run({"/proc/self/exe", "--thumbnail-worker", std::to_string(side),
             std::to_string(static_cast<std::uint64_t>(options.maxMegapixels) * 1'000'000U),
             std::to_string(static_cast<std::uint64_t>(options.maxFileMiB) * 1024U * 1024U), output.native()},
            sourceFd, cancellable, outputLimit);
  } else if (mime == "application/pdf") {
    success =
        run({"pdftoppm", "-f", "1", "-singlefile", "-scale-to", std::to_string(side), "-png", "-q", "/proc/self/fd/3",
             (directory / "result").native()},
            sourceFd, cancellable);
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
    if (video)
      arguments.insert(arguments.end(), {"-skip_frame:v", "nokey"});
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
    success = run(std::move(arguments), sourceFd, cancellable);
  }
  if (!success)
    return std::unexpected("no supported thumbnail provider");
  auto encoded = readThumbnailFile(output, outputLimit);
  if (!encoded)
    return std::unexpected("thumbnail provider exceeded output limits");
  if (mime.starts_with("image/"))
    return readThumbnailWorkerResult(std::move(*encoded), side);
  auto decoded = readThumbnailPng(*encoded, side);
  if (!decoded)
    return std::unexpected(decoded.error());
  return std::move(decoded->image);
}
