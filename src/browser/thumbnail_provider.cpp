#include "browser/thumbnail_provider.h"

#include "browser/thumbnail_codec.h"
#include "browser/thumbnail_store.h"
#include "fs/gio_ptr.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>

namespace {
  void childSetup(gpointer parent) {
    (void)::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() != GPOINTER_TO_INT(parent))
      ::_exit(1);
    (void)::setsid();
    errno = 0;
    if (::nice(10) == -1 && errno != 0)
      ::_exit(1);
    const rlimit cpu{5, 5}, memory{768U * 1024U * 1024U, 768U * 1024U * 1024U},
        file{8U * 1024U * 1024U, 8U * 1024U * 1024U};
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
  bool run(std::vector<std::string> args, int input, GCancellable* cancellable) {
    const files::GString executable(g_find_program_in_path(args.front().c_str()));
    if (!executable)
      return false;
    args.front() = executable.get();
    const files::GioPtr<GSubprocessLauncher> launcher(g_subprocess_launcher_new(
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE)
    ));
    g_subprocess_launcher_set_child_setup(launcher.get(), childSetup, GINT_TO_POINTER(::getpid()), nullptr);
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
    bool done = false;
    const files::GioPtr<GCancellable> waitCancel(g_cancellable_new());
    std::thread watchdog([&] {
      while (!g_cancellable_is_cancelled(waitCancel.get())) {
        if (g_cancellable_is_cancelled(cancellable) || std::chrono::steady_clock::now() >= deadline) {
          g_subprocess_force_exit(process.get());
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    });
    done = g_subprocess_wait_check(process.get(), nullptr, nullptr);
    g_cancellable_cancel(waitCancel.get());
    watchdog.join();
    return done && !g_cancellable_is_cancelled(cancellable);
  }
} // namespace

std::expected<LoadedImageFile, std::string> files::renderThumbnailProvider(
    int sourceFd, const std::string& mime, int side, const ThumbnailOptions& options, GCancellable* cancellable
) {
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
  const auto output = directory / "result.png";
  bool success = false;
  if (mime.starts_with("image/")) {
    success =
        run({"/proc/self/exe", "--thumbnail-worker", std::to_string(side),
             std::to_string(static_cast<std::uint64_t>(options.maxMegapixels) * 1'000'000U),
             std::to_string(static_cast<std::uint64_t>(options.maxFileMiB) * 1024U * 1024U), output.native()},
            sourceFd, cancellable);
  } else if (mime == "application/pdf") {
    success =
        run({"pdftoppm", "-f", "1", "-singlefile", "-scale-to", std::to_string(side), "-png", "-q", "/proc/self/fd/3",
             (directory / "result").native()},
            sourceFd, cancellable);
  } else if (mime.starts_with("video/") || mime.starts_with("audio/")) {
    const auto probe = directory / "probe.json";
    if (!run(
            {"ffprobe", "-v", "error", "-protocol_whitelist", "file,pipe", "-show_entries", "stream=width,height",
             "-of", "json", "-o", probe.native(), "/proc/self/fd/3"},
            sourceFd, cancellable
        ))
      return std::unexpected("media probing unavailable");
    const auto bytes = readThumbnailFile(probe, 128U * 1024U);
    if (!bytes)
      return std::unexpected("invalid media metadata");
    try {
      const auto info = nlohmann::json::parse(bytes->begin(), bytes->end());
      if (!info.contains("streams") || !info["streams"].is_array() || info["streams"].size() > 128)
        return std::unexpected("invalid media streams");
      for (const auto& stream : info["streams"]) {
        const auto width = stream.value("width", std::uint64_t{0}), height = stream.value("height", std::uint64_t{0});
        if (height && width > static_cast<std::uint64_t>(options.maxMegapixels) * 1'000'000U / height)
          return std::unexpected("media dimensions exceed thumbnail limits");
      }
    } catch (const std::exception&) {
      return std::unexpected("invalid media metadata");
    }
    success =
        run({"ffmpeg",
             "-nostdin",
             "-v",
             "error",
             "-threads",
             "1",
             "-filter_threads",
             "1",
             "-protocol_whitelist",
             "file,pipe",
             "-i",
             "/proc/self/fd/3",
             "-map",
             "0:v:0",
             "-frames:v",
             "1",
             "-an",
             "-sn",
             "-vf",
             "thumbnail=4,scale="
                 + std::to_string(side)
                 + ':'
                 + std::to_string(side)
                 + ":force_original_aspect_ratio=decrease",
             "-y",
             output.native()},
            sourceFd, cancellable);
  }
  if (!success)
    return std::unexpected("no supported thumbnail provider");
  const auto encoded = readThumbnailFile(output, 8U * 1024U * 1024U);
  if (!encoded)
    return std::unexpected("thumbnail provider exceeded output limits");
  auto decoded = readThumbnailPng(*encoded, side);
  if (!decoded)
    return std::unexpected(decoded.error());
  return std::move(decoded->image);
}
