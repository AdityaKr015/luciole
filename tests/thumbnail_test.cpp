#include "app/glib_poll_source.h"
#include "browser/thumbnail_provider.h"
#include "browser/thumbnail_service.h"
#include "browser/thumbnail_store.h"
#include "browser/thumbnail_worker.h"
#include "core/log.h"
#include "render/core/image_decoder.h"
#include "render/core/image_orientation.h"
#include "render/core/image_resize.h"

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <print>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <webp/encode.h>

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;
  using files::GioPtr;
  void require(bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  }
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto limit = std::chrono::steady_clock::now() + 20s;
      while (!done()) {
        require(std::chrono::steady_clock::now() < limit, "thumbnail request timed out");
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        require(::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) >= 0, "poll failed");
        m_source.dispatch(fds, start);
      }
    }
    void drain() {
      const auto untilTime = std::chrono::steady_clock::now() + 150ms;
      until([&] { return std::chrono::steady_clock::now() >= untilTime; });
    }

  private:
    GlibPollSource m_source;
  };
  struct Fixture {
    fs::path root;
    Fixture() {
      const files::GString temporary(g_dir_make_tmp("noctalia-thumbnails-test-XXXXXX", nullptr));
      require(temporary != nullptr, "cannot create thumbnail fixture");
      root = temporary.get();
      fs::create_directory(root / "home");
      ::setenv("HOME", (root / "home").c_str(), 1);
      ::setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);
      ::setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
      ::setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
      ::setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/noctalia-thumbnail-test-no-session", 1);
      ::setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/noctalia-thumbnail-test-no-system", 1);
    }
    ~Fixture() {
      std::error_code ignored;
      fs::remove_all(root, ignored);
    }
  };
  LoadedImageFile picture(int width = 32, int height = 16) {
    LoadedImageFile image{
        .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4),
        .width = width,
        .height = height
    };
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
        image.rgba[offset] = x < width / 2 ? 240 : 20;
        image.rgba[offset + 1] = x < width / 2 ? 20 : 240;
        image.rgba[offset + 2] = 40;
        image.rgba[offset + 3] = y < height / 2 ? 128 : 255;
      }
    return image;
  }
  void write(const fs::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(stream), "cannot write image fixture");
  }
  std::vector<std::uint8_t> png(const LoadedImageFile& image) {
    auto bytes = files::writeThumbnailPng({.image = image, .text = {}});
    require(bytes.has_value(), "cannot encode PNG fixture");
    return std::move(*bytes);
  }
  files::DirectoryEntry source(const fs::path& path, Pump& pump) {
    files::DirectorySession session;
    session.open(files::Location::fromPath(path.parent_path().native()));
    pump.until([&] { return session.snapshot().status != files::DirectoryStatus::LOADING; });
    require(session.snapshot().status == files::DirectoryStatus::READY, "cannot list thumbnail fixture");
    for (const auto& entry : session.snapshot().entries)
      if (entry.name == path.filename().native())
        return entry;
    throw std::runtime_error("missing thumbnail fixture");
  }
  void testCodecs() {
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const auto original = picture();
    auto data = png(original);
    auto decoded = files::decodeThumbnail(data, 128, 1000000, cancel.get());
    require(
        decoded && decoded->rgba == original.rgba && decoded->width == 32 && decoded->height == 16,
        "PNG lost alpha, aspect ratio or original pixels"
    );
    require(!files::decodeThumbnail(data, 128, 100, cancel.get()), "oversized source bypassed pixel limits");
    auto small = files::decodeThumbnail(data, 8, 1000000, cancel.get());
    require(
        small && small->width == 8 && small->height == 4 && small->rgba[3] >= 127 && small->rgba[3] <= 129,
        "downscaling lost aspect or transparency"
    );
    // Exercise the full-size output allowed by system loaders, independently of their scaling support.
    const GioPtr<GdkPixbuf> fullSize(gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 1600, 900));
    gdk_pixbuf_fill(fullSize.get(), 0x20408080U);
    auto fitted = files::fitThumbnailPixels(fullSize.get(), 512, 2'000'000);
    require(
        fitted && fitted->width == 512 && fitted->height == 288 && fitted->rgba[0] == 32 && fitted->rgba[3] == 128,
        "full-size decoder output was not bounded with alpha and aspect preserved"
    );
    require(
        !files::fitThumbnailPixels(fullSize.get(), 512, 1'000'000), "post-decode scaling bypassed source pixel limits"
    );
    const std::string svg =
        "<svg xmlns='http://www.w3.org/2000/svg'><image href='https://invalid.example/a.png'/></svg>";
    require(
        !files::decodeThumbnail(
            {reinterpret_cast<const std::uint8_t*>(svg.data()), svg.size()}, 128, 1000000, cancel.get()
        ),
        "SVG resolved external resources through the raster decoder"
    );
    std::uint8_t* webp = nullptr;
    const auto length =
        WebPEncodeLosslessRGBA(original.rgba.data(), original.width, original.height, original.width * 4, &webp);
    require(length > 0, "cannot create WebP fixture");
    decoded = files::decodeThumbnail({webp, length}, 128, 1000000, cancel.get());
    WebPFree(webp);
    require(decoded && decoded->rgba == original.rgba, "WebP lost transparency or pixels");
    const GioPtr<GdkPixbuf> pixbuf(gdk_pixbuf_new(GDK_COLORSPACE_RGB, false, 8, 32, 16));
    gdk_pixbuf_fill(pixbuf.get(), 0xff3020ffU);
    gchar* jpeg = nullptr;
    gsize jpegSize = 0;
    require(
        gdk_pixbuf_save_to_buffer(pixbuf.get(), &jpeg, &jpegSize, "jpeg", nullptr, "quality", "100", nullptr),
        "JPEG encoder unavailable"
    );
    std::vector<std::uint8_t> oriented(
        reinterpret_cast<std::uint8_t*>(jpeg), reinterpret_cast<std::uint8_t*>(jpeg) + jpegSize
    );
    g_free(jpeg);
    const std::array<std::uint8_t, 36> exif{0xff, 0xe1, 0, 34, 'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0, 8, 0, 0, 0, 1,
                                            0,    0x12, 1, 3,  0,   1,   0,   0,   0, 6, 0,   0,   0,  0, 0, 0, 0};
    oriented.insert(oriented.begin() + 2, exif.begin(), exif.end());
    decoded = files::decodeThumbnail(oriented, 128, 1000000, cancel.get());
    require(decoded && decoded->width == 16 && decoded->height == 32, "JPEG EXIF orientation was not applied");
    require(!files::readThumbnailPng(std::span(data).first(20), 128), "truncated PNG was accepted");
    data[20] ^= 1;
    require(!files::readThumbnailPng(data, 128), "corrupt PNG CRC was ignored");
    g_cancellable_cancel(cancel.get());
    require(!files::decodeThumbnail(png(original), 128, 1000000, cancel.get()), "cancelled decode completed");
  }
  void testRasterFormats() {
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const std::vector<std::uint8_t> gif{'G', 'I',  'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0, 0,    0, 0, 255, 255,
                                        255, 0x2c, 0,   0,   0,   0,   1, 0, 1, 0, 0,    2, 2, 0x44, 1, 0, 0x3b};
    auto decoded = files::decodeThumbnail(gif, 128, 1000000, cancel.get());
    require(
        decoded && decoded->width == 1 && decoded->height == 1 && decoded->rgba[3] == 255,
        "raster decoder did not decode the first GIF frame"
    );
    const std::vector<std::uint8_t> pnm{'P', '6', '\n', '2', ' ', '1', '\n', '2', '5', '5', '\n', 255, 0, 0, 0, 255, 0};
    decoded = files::decodeThumbnail(pnm, 128, 1000000, cancel.get());
    require(
        decoded && decoded->width == 2 && decoded->height == 1 && decoded->rgba[0] == 255 && decoded->rgba[5] == 255,
        "raster decoder lost PNM pixels"
    );
    std::vector<std::uint8_t> bmp(62);
    const auto put32 = [](auto& data, std::size_t offset, std::uint32_t value) {
      for (std::size_t i = 0; i < 4; ++i)
        data[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
    };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(bmp, 2, 62);
    put32(bmp, 10, 54);
    put32(bmp, 14, 40);
    put32(bmp, 18, 2);
    put32(bmp, 22, 1);
    bmp[26] = 1;
    bmp[28] = 24;
    bmp[56] = 255;
    bmp[58] = 255;
    decoded = files::decodeThumbnail(bmp, 128, 1000000, cancel.get());
    require(
        decoded && decoded->width == 2 && decoded->rgba[0] == 255 && decoded->rgba[5] == 255,
        "raster decoder lost BMP channel order"
    );
    const auto pngData = png(picture());
    std::vector<std::uint8_t> ico(22);
    ico[2] = 1;
    ico[4] = 1;
    ico[6] = 32;
    ico[7] = 16;
    ico[10] = 1;
    ico[12] = 32;
    put32(ico, 14, static_cast<std::uint32_t>(pngData.size()));
    put32(ico, 18, 22);
    ico.insert(ico.end(), pngData.begin(), pngData.end());
    decoded = files::decodeThumbnail(ico, 128, 1000000, cancel.get());
    require(decoded && decoded->rgba == picture().rgba, "ICO loader lost embedded PNG alpha");
    // Verify both PNG-backed and 32-bit DIB icon transparency.
    ico.resize(22);
    ico[6] = 2;
    ico[7] = 1;
    put32(ico, 14, 52);
    std::vector<std::uint8_t> dib(52);
    put32(dib, 0, 40);
    put32(dib, 4, 2);
    put32(dib, 8, 2);
    dib[12] = 1;
    dib[14] = 32;
    dib[42] = 255;
    dib[43] = 128;
    dib[45] = 255;
    dib[47] = 255;
    ico.insert(ico.end(), dib.begin(), dib.end());
    decoded = files::decodeThumbnail(ico, 128, 1000000, cancel.get());
    require(
        decoded
            && decoded->width == 2
            && decoded->height == 1
            && decoded->rgba[0] == 255
            && decoded->rgba[3] == 128
            && decoded->rgba[5] == 255,
        "ICO DIB loader lost alpha or channel order"
    );
    require(!files::decodeThumbnail(ico, 128, 1, cancel.get()), "ICO bypassed source pixel limits");
    put32(ico, 26, 0x80000000U);
    require(!files::decodeThumbnail(ico, 128, 1000000, cancel.get()), "ICO accepted invalid signed dimensions");
    const std::array<std::uint8_t, 8> transparentRedBlue{255, 0, 0, 0, 0, 0, 255, 255};
    const auto resized = resizeToRgba(transparentRedBlue.data(), 2, 1, ResizeSourceLayout::Rgba, 1, 1);
    require(
        resized.size() == 4 && resized[0] < 5 && resized[2] > 250 && resized[3] >= 125 && resized[3] <= 130,
        "Noctalia resampling introduced a transparent-edge halo"
    );
    auto corrupt = pngData;
    corrupt[20] ^= 1;
    require(!files::decodeThumbnail(corrupt, 128, 1000000, cancel.get()), "raster decoder ignored PNG header checksum");
    require(!decodeRasterImage(pngData.data(), pngData.size(), 100), "raster decoder allocated an oversized image");
    const auto theme = decodeRasterImage(pngData.data(), pngData.size());
    require(theme && theme->pixels == picture().rgba, "theme PNG artwork did not decode");
  }
  void benchmarkThumbnails(const fs::path& directory, std::size_t limit, bool videos) {
    require(limit >= 1 && limit <= 256, "benchmark count must be between 1 and 256");
    Pump pump;
    files::DirectorySession session;
    session.open(files::Location::fromPath(fs::absolute(directory).native()));
    pump.until([&] { return session.snapshot().status != files::DirectoryStatus::LOADING; });
    require(session.snapshot().status == files::DirectoryStatus::READY, "cannot read benchmark directory");
    auto entries = session.snapshot().entries;
    std::erase_if(entries, [videos](const auto& entry) {
      return entry.type != G_FILE_TYPE_REGULAR
          || !entry.contentType
          || !entry.contentType->starts_with(videos ? "video/" : "image/")
          || entry.symlink;
    });
    std::ranges::sort(entries, {}, &files::DirectoryEntry::name);
    if (entries.size() > limit)
      entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(limit), entries.end());
    require(!entries.empty(), videos ? "benchmark requires video files" : "benchmark requires image files");
    files::ThumbnailService service;
    const auto phase = [&](const char* name) {
      std::vector<std::shared_ptr<const files::FileIconImage>> assets;
      assets.reserve(entries.size());
      std::vector<double> elapsed(entries.size(), -1);
      const auto started = std::chrono::steady_clock::now();
      for (const auto& entry : entries)
        assets.push_back(service.request(entry, 128));
      pump.until([&] {
        bool done = true;
        const auto now = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < assets.size(); ++i) {
          require(assets[i] != nullptr, "benchmark request exceeded admission limits");
          if (!assets[i]->ready)
            done = false;
          else if (elapsed[i] < 0)
            elapsed[i] = std::chrono::duration<double, std::milli>(now - started).count();
        }
        return done;
      });
      require(
          std::ranges::all_of(assets, [](const auto& image) { return image->pixels.has_value(); }),
          "benchmark input has unsupported, invalid or oversized media"
      );
      std::ranges::sort(elapsed);
      std::println(
          "{} count={} first={:.2f} median={:.2f} last={:.2f} ms", name, entries.size(), elapsed.front(),
          elapsed[elapsed.size() / 2], elapsed.back()
      );
    };
    phase("cold-thumbnail-cache");
    const auto cache = files::ThumbnailStore{}.root() / "normal";
    pump.until([&] {
      return std::ranges::all_of(entries, [&](const auto& entry) {
        return fs::exists(cache / files::ThumbnailStore::filename(entry.location.uri()));
      });
    });
    service.invalidate();
    phase("warm-disk");
    phase("warm-memory");
  }
  void testWorkerTransport() {
    std::vector<std::uint8_t> valid{'N', 'F', 'R', '1', 0, 0, 0, 1, 0, 0, 0, 1, 240, 20, 40, 128};
    const auto result = files::readThumbnailWorkerResult(valid, 128);
    require(
        result
            && result->width == 1
            && result->height == 1
            && result->rgba == std::vector<std::uint8_t>({240, 20, 40, 128}),
        "private worker transport lost pixels or alpha"
    );
    auto malformed = valid;
    malformed[0] = 'X';
    require(!files::readThumbnailWorkerResult(malformed, 128), "worker transport accepted invalid magic");
    malformed = valid;
    malformed.pop_back();
    require(!files::readThumbnailWorkerResult(malformed, 128), "worker transport accepted truncated pixels");
    malformed = valid;
    malformed.push_back(0);
    require(!files::readThumbnailWorkerResult(malformed, 128), "worker transport accepted trailing bytes");
    malformed = valid;
    malformed[4] = 1;
    require(!files::readThumbnailWorkerResult(malformed, 128), "worker transport accepted oversized dimensions");
    require(!files::readThumbnailWorkerResult(valid, 0), "worker transport accepted an invalid size limit");
    const std::string diagnostic = "NFE1GdkPixbuf: decoder unavailable";
    const auto failed = files::readThumbnailWorkerResult({diagnostic.begin(), diagnostic.end()}, 128);
    require(!failed && failed.error() == diagnostic.substr(4), "worker lost its decoder diagnostic");
    auto oversized = std::vector<std::uint8_t>(1025, 'a');
    std::copy_n(diagnostic.begin(), 4, oversized.begin());
    require(
        files::readThumbnailWorkerResult(oversized, 128).error() != std::string(1021, 'a'),
        "worker accepted an oversized diagnostic"
    );
  }
  void testBatchAndLargerCache() {
    files::ThumbnailStore store;
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const auto uri = files::Location::fromPath("/tmp/cache-larger-only.png").uri();
    const auto second = files::Location::fromPath("/tmp/cache-batch-second.png").uri();
    const auto make = [](const std::string& key) {
      return files::ThumbnailPng{
          .image = picture(256, 128),
          .text = {
              {"Thumb::URI", key}, {"Thumb::MTime", "123"}, {"Thumb::Size", "42"}, {"X-Noctalia-Files::Stamp", "same"}
          }
      };
    };
    const std::array writes{files::ThumbnailCacheWrite{make(uri), 256}, files::ThumbnailCacheWrite{make(second), 512}};
    store.writeBatch(writes, 16U * 1024U * 1024U, cancel.get());
    const auto cached = store.read(uri, 128, 123, 42, "same");
    require(
        cached
            && cached->image.width == 128
            && cached->image.height == 64
            && cached->image.rgba[0] == 240
            && cached->image.rgba[3] == 128,
        "larger cache reuse lost bounds, aspect or alpha"
    );
    require(
        store.read(second, 128, 123, 42, "same").has_value() && !store.read(uri, 128, 123, 42, "changed"),
        "batch cache lost an item or accepted a stale larger image"
    );
    require(store.clear().empty(), "batch cache clearing failed");
  }
  void testCache() {
    files::ThumbnailStore store;
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const auto uri = files::Location::fromPath("/tmp/literal space\nimage.png").uri();
    const auto make = [&](const std::string& key) {
      return files::ThumbnailPng{
          .image = picture(), .text = {{"Thumb::URI", key}, {"Thumb::MTime", "123"}, {"Thumb::Size", "42"}}
      };
    };
    store.write(make(uri), 128, 16U * 1024U * 1024U, cancel.get());
    const auto path = store.root() / "normal" / files::ThumbnailStore::filename(uri);
    require(
        fs::exists(path) && files::ThumbnailStore::filename("abc") == "900150983cd24fb0d6963f7d28e17f72.png",
        "cache is not URI/MD5 keyed under the standard class"
    );
    const auto cached = store.read(uri, 128, 123, 42);
    require(cached && cached->image.rgba == picture().rgba, "standard cache PNG did not round-trip");
    const GioPtr<GdkPixbuf> independent(gdk_pixbuf_new_from_file(path.c_str(), nullptr));
    const char* tag = independent ? gdk_pixbuf_get_option(independent.get(), "tEXt::Thumb::URI") : nullptr;
    require(tag && uri == tag, "another PNG reader could not read standard thumbnail metadata");
    require(
        !store.read(uri, 128, 124, 42) && !store.read(uri, 128, 122, 42) && !store.read(uri, 128, 123, 43),
        "cache accepted stale metadata"
    );
    struct stat mode{};
    require(::stat(path.c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0600, "thumbnail permissions are not private");
    auto foreign = make("sftp://example.invalid/photo.png");
    auto bytes = files::writeThumbnailPng(foreign);
    require(bytes.has_value(), "cannot encode foreign cache fixture");
    const auto foreignPath = store.root() / "normal" / files::ThumbnailStore::filename(foreign.text.at("Thumb::URI"));
    write(foreignPath, *bytes);
    require(
        store.clear().empty() && !fs::exists(path) && fs::exists(foreignPath),
        "cache clearing removed foreign data or retained owned data"
    );
    store.write(make(uri), 256, 16U * 1024U * 1024U, cancel.get());
    const auto large = store.root() / "large" / files::ThumbnailStore::filename(uri);
    require(fs::exists(large), "large thumbnail class was not written");
    fs::rename(large, large.string() + ".old");
    write(large, *bytes);
    require(
        store.clear().empty() && fs::exists(large), "clearing deleted a foreign replacement at a previously owned name"
    );
    auto bad = make(uri);
    bad.text.erase("Thumb::MTime");
    bytes = files::writeThumbnailPng(bad);
    require(bytes.has_value(), "cannot encode missing metadata fixture");
    write(path, *bytes);
    require(!store.read(uri, 128, 123, 42), "cache accepted missing mandatory mtime");
  }
  void testCacheLimits() {
    files::ThumbnailStore store;
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const auto make = [](const std::string& uri, const std::string& stamp) {
      return files::ThumbnailPng{
          .image = picture(),
          .text = {
              {"Thumb::URI", uri}, {"Thumb::MTime", "123"}, {"Thumb::Size", "42"}, {"X-Noctalia-Files::Stamp", stamp}
          }
      };
    };
    const auto first = files::Location::fromPath("/tmp/cache-one.png").uri();
    const auto second = files::Location::fromPath("/tmp/cache-two.png").uri();
    store.write(make(first, "source-one"), 128, 1000000, cancel.get());
    require(
        store.read(first, 128, 123, 42, "source-one").has_value() && !store.read(first, 128, 123, 42, "source-two"),
        "cache did not reject a same-size/mtime source revision"
    );
    const auto one = store.root() / "normal" / files::ThumbnailStore::filename(first);
    const auto budget = fs::file_size(one) + 32;
    store.write(make(second, "source-two"), 128, budget, cancel.get());
    const auto two = store.root() / "normal" / files::ThumbnailStore::filename(second);
    require(
        !fs::exists(one) && fs::exists(two) && fs::file_size(two) <= budget, "disk cache exceeded its pruning budget"
    );
    require(store.clear().empty(), "cannot clear cache budget fixture");
  }
  void command(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args)
      argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    const GioPtr<GSubprocess> process(g_subprocess_newv(
        argv.data(),
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), nullptr
    ));
    require(process && g_subprocess_wait_check(process.get(), nullptr, nullptr), "cannot generate real media fixture");
  }
  void testProviders(const Fixture& fixture) {
    const auto pngPath = fixture.root / "artwork.png";
    write(pngPath, png(picture()));
    const auto video = fixture.root / "clip.mp4";
    command(
        {"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i", "color=c=red:s=96x48:d=0.2", "-threads", "1", "-c:v",
         "mpeg4", video.native()}
    );
    const auto shortVideo = fixture.root / "single-frame.mp4";
    command(
        {"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i", "color=c=red:s=96x48:r=30", "-frames:v", "1", "-c:v",
         "libx264", "-threads", "1", "-preset", "ultrafast", shortVideo.native()}
    );
    const auto longGop = fixture.root / "long-gop.mp4";
    command(
        {"ffmpeg",
         "-nostdin",
         "-v",
         "error",
         "-f",
         "lavfi",
         "-i",
         "color=c=blue:s=192x96:r=30",
         "-frames:v",
         "60",
         "-c:v",
         "libx264",
         "-threads",
         "1",
         "-preset",
         "veryfast",
         "-g",
         "120",
         "-keyint_min",
         "120",
         "-bf",
         "3",
         "-sc_threshold",
         "0",
         longGop.native()}
    );
    const auto rotatedVideo = fixture.root / "rotated.mp4";
    command(
        {"ffmpeg", "-nostdin", "-v", "error", "-display_rotation:v:0", "90", "-i", longGop.native(), "-c", "copy",
         rotatedVideo.native()}
    );
    const auto intraVideo = fixture.root / "intra.avi";
    command(
        {"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i", "color=c=blue:s=192x96:d=0.12", "-c:v", "mjpeg",
         "-threads", "1", intraVideo.native()}
    );
    const auto coveredVideo = fixture.root / "covered.mp4";
    command(
        {"ffmpeg",
         "-nostdin",
         "-v",
         "error",
         "-i",
         pngPath.native(),
         "-i",
         longGop.native(),
         "-map",
         "0:v",
         "-map",
         "1:v",
         "-c:v:0",
         "png",
         "-disposition:v:0",
         "attached_pic",
         "-c:v:1",
         "copy",
         "-disposition:v:1",
         "default",
         "-threads",
         "1",
         coveredVideo.native()}
    );
    const auto largeVideo = fixture.root / "large.mp4";
    command(
        {"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i", "color=c=red:s=2048x1024:d=0.1", "-threads", "1",
         "-c:v", "mpeg4", largeVideo.native()}
    );
    const int largeInput = ::open(largeVideo.c_str(), O_RDONLY | O_CLOEXEC);
    require(largeInput >= 0, "cannot open oversized media fixture");
    const GioPtr<GCancellable> limitCancel(g_cancellable_new());
    auto limited = files::ThumbnailOptions{};
    limited.maxMegapixels = 1;
    const auto tooLarge = files::renderThumbnailProvider(largeInput, "video/mp4", 128, limited, limitCancel.get());
    ::close(largeInput);
    require(!tooLarge, "media decoder bypassed its source-pixel limit");
    const auto audio = fixture.root / "artwork.mp3";
    command(
        {"ffmpeg",
         "-nostdin",
         "-v",
         "error",
         "-f",
         "lavfi",
         "-i",
         "sine=duration=0.2",
         "-i",
         pngPath.native(),
         "-map",
         "0:a",
         "-map",
         "1:v",
         "-c:a",
         "libmp3lame",
         "-c:v",
         "png",
         "-threads",
         "1",
         "-disposition:v",
         "attached_pic",
         audio.native()}
    );
    const auto plain = fixture.root / "plain.wav";
    command({"ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i", "sine=duration=0.1", plain.native()});
    const auto document = fixture.root / "page.pdf";
    std::string pdf = "%PDF-1.4\n";
    std::vector<std::size_t> offsets{0};
    const std::string drawing = "1 0 0 rg 0 0 160 80 re f\n";
    const std::array objects{
        std::string("<< /Type /Catalog /Pages 2 0 R >>"), std::string("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"),
        std::string("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 160 80] /Contents 4 0 R >>"),
        "<< /Length " + std::to_string(drawing.size()) + " >>\nstream\n" + drawing + "endstream"
    };
    for (const auto& object : objects) {
      offsets.push_back(pdf.size());
      pdf += std::to_string(offsets.size() - 1) + " 0 obj\n" + object + "\nendobj\n";
    }
    const auto xref = pdf.size();
    pdf += "xref\n0 5\n0000000000 65535 f \n";
    for (std::size_t i = 1; i < offsets.size(); ++i) {
      const auto value = std::to_string(offsets[i]);
      pdf += std::string(10 - value.size(), '0') + value + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    std::ofstream(document, std::ios::binary) << pdf;
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const auto render = [&](const fs::path& path, const std::string& mime) {
      const int input = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
      require(input >= 0, "cannot open provider fixture");
      auto result = files::renderThumbnailProvider(input, mime, 128, {}, cancel.get());
      ::close(input);
      return result;
    };
    const auto corrupt = fixture.root / "broken.png";
    std::ofstream(corrupt, std::ios::binary) << "not an image";
    const auto failedImage = render(corrupt, "image/png");
    require(
        !failedImage && failedImage.error().contains("GdkPixbuf") && !failedImage.error().contains(corrupt.native()),
        "isolated decoder lost its bounded diagnostic or exposed the source path"
    );
    const auto rasterSource = fixture.root / "format-source.png";
    write(rasterSource, png(picture(128, 64)));
    const auto jxl = fixture.root / "format.jxl";
    // Reuse Noctalia's synthetic lossless JPEG XL fixture.
    constexpr std::array<std::uint8_t, 100> jxlBytes = {
        0xFF, 0x0A, 0x18, 0x10, 0xB0, 0x12, 0x08, 0x10, 0x10, 0x00, 0x60, 0x01, 0x4B, 0x28, 0x24, 0xC6, 0x15,
        0x0A, 0x0A, 0x12, 0x4E, 0x60, 0x4C, 0x19, 0x7B, 0x62, 0x72, 0x01, 0x81, 0x00, 0x24, 0xE7, 0x87, 0x96,
        0x2F, 0xBF, 0x63, 0x00, 0xF7, 0x27, 0x80, 0xF3, 0x53, 0x3F, 0xF9, 0x23, 0x39, 0x92, 0x24, 0x39, 0xF7,
        0x27, 0x00, 0xEA, 0x27, 0x7F, 0x40, 0x72, 0x24, 0x49, 0x3A, 0xF6, 0xFD, 0xF3, 0xAF, 0x1F, 0xF2, 0xAF,
        0x3F, 0xFF, 0xFA, 0xF3, 0xDF, 0x9D, 0x7F, 0xFD, 0xBB, 0x77, 0x1F, 0x3C, 0x00, 0x02, 0xE0, 0xFC, 0xE4,
        0x4F, 0xFE, 0x48, 0x1F, 0x93, 0xF1, 0xF3, 0x8C, 0xDD, 0x09, 0x13, 0x61, 0xC6, 0xF3, 0x05,
    };
    constexpr std::array<std::uint8_t, 64> jxlRgba = {
        255, 0,   0,   255, 0,   255, 0,  255, 0,  0,   255, 255, 255, 255, 0,  255, 255, 0,   255, 255, 0,   255,
        255, 255, 255, 255, 255, 255, 0,  0,   0,  255, 128, 0,   0,   128, 0,  128, 0,   128, 0,   0,   128, 128,
        128, 128, 0,   128, 10,  20,  30, 0,   40, 50,  60,  0,   70,  80,  90, 0,   100, 110, 120, 0,
    };
    write(jxl, jxlBytes);
    auto richImage = render(jxl, "image/jxl");
    if (!richImage)
      std::println(stderr, "JPEG XL fixture error: {}", richImage.error());
    require(
        richImage
            && richImage->width == 4
            && richImage->height == 4
            && richImage->rgba == std::vector<std::uint8_t>(jxlRgba.begin(), jxlRgba.end()),
        "Glycin JPEG XL decode lost size, color or alpha"
    );
    const auto avif = fixture.root / "format.avif";
    command(
        {"ffmpeg", "-nostdin", "-v", "error", "-i", rasterSource.native(), "-frames:v", "1", "-c:v", "libaom-av1",
         "-cpu-used", "8", "-still-picture", "1", "-pix_fmt", "yuv444p", "-threads", "1", avif.native()}
    );
    richImage = render(avif, "image/avif");
    require(
        richImage && richImage->width == 128 && richImage->height == 64 && richImage->rgba[0] > 180,
        "Glycin AVIF loader did not decode the expected image"
    );
    const auto animated = fixture.root / "animated.webp";
    command(
        {"ffmpeg",
         "-nostdin",
         "-v",
         "error",
         "-f",
         "lavfi",
         "-i",
         "color=c=red:s=64x32:r=2:d=0.5",
         "-f",
         "lavfi",
         "-i",
         "color=c=blue:s=64x32:r=2:d=0.5",
         "-filter_complex",
         "[0:v][1:v]concat=n=2:v=1:a=0",
         "-filter_complex_threads",
         "1",
         "-c:v",
         "libwebp_anim",
         "-lossless",
         "1",
         "-loop",
         "0",
         "-threads",
         "1",
         animated.native()}
    );
    richImage = render(animated, "image/webp");
    require(
        richImage
            && richImage->width == 64
            && richImage->height == 32
            && richImage->rgba[0] > 220
            && richImage->rgba[2] < 30,
        "animated WebP did not use its first frame"
    );
    auto still = render(video, "video/mp4");
    require(
        still && still->width == 128 && still->height == 64 && still->rgba.front() > 220,
        "video provider did not render the expected still"
    );
    still = render(shortVideo, "video/mp4");
    require(
        still && still->width == 128 && still->height == 64 && still->rgba[0] > 220,
        "single-frame video did not produce a thumbnail"
    );
    still = render(longGop, "video/mp4");
    require(
        still && still->width == 128 && still->height == 64 && still->rgba[2] > 220,
        "long-GOP B-frame video did not produce its actual frame"
    );
    still = render(rotatedVideo, "video/mp4");
    require(
        still && still->width == 64 && still->height == 128 && still->rgba[2] > 220,
        "video display rotation or aspect was lost"
    );
    still = render(intraVideo, "video/x-msvideo");
    require(
        still && still->width == 128 && still->height == 64 && still->rgba[2] > 220,
        "intra-only video did not produce its actual frame"
    );
    still = render(coveredVideo, "video/mp4");
    require(
        still && still->width == 128 && still->height == 64 && still->rgba[2] > 220 && still->rgba[0] < 30,
        "video thumbnail selected attached artwork instead of a video frame"
    );
    still = render(document, "application/pdf");
    require(
        still && still->width == 128 && still->height == 64 && still->rgba.front() > 220,
        "PDF provider did not render the first page"
    );
    still = render(audio, "audio/mpeg");
    require(still && still->width == 128 && still->height == 64, "audio provider did not extract artwork");
    require(!render(plain, "audio/x-wav"), "audio without artwork fabricated a thumbnail");
    const std::string originalPath = std::getenv("PATH");
    ::setenv("PATH", "/noctalia-thumbnail-test-no-tools", 1);
    require(
        !render(document, "application/pdf") && !render(video, "video/mp4"), "unavailable providers did not fall back"
    );
    ::setenv("PATH", originalPath.c_str(), 1);
    const auto bin = fixture.root / "slow-bin";
    fs::create_directory(bin);
    const files::GString sleep(g_find_program_in_path("sleep"));
    require(sleep != nullptr, "missing cancellation fixture helper");
    const files::GString shell(g_find_program_in_path("sh"));
    require(shell != nullptr, "missing cancellation fixture shell");
    std::ofstream(bin / "pdftoppm") << "#!" << shell.get() << "\nexec " << sleep.get() << " 30\n";
    require(::chmod((bin / "pdftoppm").c_str(), 0700) == 0, "cannot enable cancellation helper");
    ::setenv("PATH", bin.c_str(), 1);
    const auto started = std::chrono::steady_clock::now();
    std::thread cancelling([&] {
      std::this_thread::sleep_for(100ms);
      g_cancellable_cancel(cancel.get());
    });
    still = render(document, "application/pdf");
    cancelling.join();
    g_cancellable_reset(cancel.get());
    const auto timeoutStarted = std::chrono::steady_clock::now();
    still = render(document, "application/pdf");
    ::setenv("PATH", originalPath.c_str(), 1);
    require(timeoutStarted - started < 2s, "cancelled helper remained alive");
    const auto elapsed = std::chrono::steady_clock::now() - timeoutStarted;
    require(!still && elapsed >= 7s && elapsed < 12s, "provider wall-clock deadline was not enforced");
  }
  void testCacheLanesAndCancellation(const Fixture& fixture) {
    Pump pump;
    const auto firstPath = fixture.root / "slow-one.pdf";
    const auto secondPath = fixture.root / "slow-two.pdf";
    fs::copy_file(fixture.root / "page.pdf", firstPath);
    fs::copy_file(fixture.root / "page.pdf", secondPath);
    const auto first = source(firstPath, pump), second = source(secondPath, pump);
    const auto warm = source(fixture.root / "artwork.png", pump);
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    files::ThumbnailStore store;
    store.write(
        {.image = picture(),
         .text =
             {{"Thumb::URI", warm.location.uri()},
              {"Thumb::MTime", std::to_string(*warm.modifiedSeconds)},
              {"Thumb::Size", std::to_string(*warm.size)}}},
        128, 16U * 1024U * 1024U, cancel.get()
    );
    const auto bin = fixture.root / "blocked-bin";
    fs::create_directory(bin);
    const files::GString shell(g_find_program_in_path("sh")), sleep(g_find_program_in_path("sleep"));
    require(shell && sleep, "missing isolated provider fixture tools");
    std::ofstream(bin / "pdftoppm") << "#!" << shell.get() << "\nexec " << sleep.get() << " 30\n";
    require(::chmod((bin / "pdftoppm").c_str(), 0700) == 0, "cannot enable isolated provider fixture");
    const std::string path = std::getenv("PATH");
    ::setenv("PATH", bin.c_str(), 1);
    files::ThumbnailService service;
    auto a = service.request(first, 128), b = service.request(second, 128);
    require(a && b, "slow provider fixtures were not admitted");
    pump.until([&] { return service.activeJobs() == 2; });
    auto cached = service.request(warm, 128);
    require(cached != nullptr, "warm cache fixture was not admitted");
    pump.until([&] { return cached->ready; });
    require(
        cached->pixels && service.activeJobs() == 2 && !a->ready && !b->ready,
        "warm cached image waited for occupied decoder slots"
    );
    a.reset();
    b.reset();
    service.endFrame();
    pump.until([&] { return service.activeJobs() == 0; });
    ::setenv("PATH", path.c_str(), 1);
    service.clearCache();
    pump.until([&] { return !service.clearing(); });
  }
  void testPublicationAndSourceChanges(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "publication.png";
    std::vector<std::uint8_t> original, alternate;
    std::map<std::size_t, std::vector<std::uint8_t>> sizes;
    int color = 0;
    for (int value = 1; value < 240; ++value) {
      auto changed = picture();
      for (std::size_t offset = 0; offset < changed.rgba.size(); offset += 4)
        changed.rgba[offset] = static_cast<std::uint8_t>(value);
      auto candidate = png(changed);
      const auto found = sizes.find(candidate.size());
      if (found != sizes.end()) {
        original = found->second;
        alternate = std::move(candidate);
        color = value;
        break;
      }
      sizes.emplace(candidate.size(), std::move(candidate));
    }
    require(!alternate.empty(), "cannot create a same-size source-edit fixture");
    write(path, original);
    auto entry = source(path, pump);
    files::ThumbnailService service;
    auto asset = service.request(entry, 128);
    require(asset != nullptr, "publication fixture denied");
    pump.until([&] { return asset->ready; });
    const auto cachePath =
        files::ThumbnailStore{}.root() / "normal" / files::ThumbnailStore::filename(entry.location.uri());
    require(asset->pixels && !fs::exists(cachePath), "decoded pixels waited for cache persistence");
    pump.until([&] { return fs::exists(cachePath); });
    files::DirectorySnapshot snapshot;
    snapshot.generation = 100;
    snapshot.location = files::Location::fromPath(fixture.root.native());
    snapshot.status = files::DirectoryStatus::READY;
    snapshot.entries.push_back(entry);
    service.synchronizeSources(snapshot);
    ++snapshot.generation;
    service.synchronizeSources(snapshot);
    require(
        service.touch(asset, true) && service.request(entry, 128) == asset && asset->pixels,
        "metadata-only publication discarded a valid thumbnail"
    );
    const auto oldMtime = fs::last_write_time(path);
    write(path, alternate);
    fs::last_write_time(path, oldMtime);
    auto updated = source(path, pump);
    require(
        updated.size == entry.size
            && updated.modifiedSeconds == entry.modifiedSeconds
            && updated.contentRevision != entry.contentRevision,
        "source-edit fixture did not preserve size/mtime or update change time"
    );
    snapshot.entries = {updated};
    ++snapshot.generation;
    service.synchronizeSources(snapshot);
    require(!service.touch(asset, true) && !asset->pixels, "source revision change retained stale pixels");
    auto replacement = service.request(updated, 128);
    pump.until([&] { return replacement->ready; });
    require(
        replacement != asset && replacement->pixels && replacement->pixels->rgba[0] == color,
        "same-size/preserved-mtime edit did not update the image"
    );
  }
  void testService(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "café\nimage \xff.png";
    write(path, png(picture()));
    auto entry = source(path, pump);
    files::ThumbnailService thumbnails;
    auto asset = thumbnails.request(entry, 48);
    require(asset != nullptr, "native thumbnail request was denied");
    pump.until([&] { return asset->ready; });
    require(
        asset->pixels && asset->pixels->width == 32 && asset->pixels->rgba[3] == 128,
        "isolated image worker did not publish correct pixels"
    );
    require(
        thumbnails.activeJobs() <= 2 && thumbnails.reservedBytes() <= 64U * 1024U * 1024U,
        "thumbnail residency/concurrency exceeded bounds"
    );
    const auto original = asset;
    const auto stampBefore = fs::last_write_time(path);
    auto replacement = picture();
    std::ranges::fill(replacement.rgba, 255);
    write(path, png(replacement));
    fs::last_write_time(path, stampBefore);
    thumbnails.invalidate();
    require(!original->pixels, "retired assets retained decoded buffers");
    entry = source(path, pump);
    asset = thumbnails.request(entry, 48);
    pump.until([&] { return asset->ready; });
    require(asset->pixels && asset->pixels->rgba.front() == 255, "preserved-mtime source edit reused stale pixels");
    auto options = files::ThumbnailOptions{};
    options.policy = files::ThumbnailPolicy::NEVER;
    thumbnails.setOptions(options);
    require(!thumbnails.request(entry, 48), "disabled policy still requested thumbnails");
    options.policy = files::ThumbnailPolicy::LOCAL;
    thumbnails.setOptions(options);
    std::vector<std::shared_ptr<const files::FileIconImage>> held;
    for (std::uint64_t id = 1; id <= 1200; ++id) {
      entry.id = id;
      if (auto image = thumbnails.request(entry, 48))
        held.push_back(std::move(image));
    }
    require(
        thumbnails.reservedBytes() <= 64U * 1024U * 1024U && held.size() < 1200 && thumbnails.queuedJobs() <= 1024,
        "pinned requests bypassed hard admission limits"
    );
    thumbnails.invalidate();
    held.clear();
    std::vector<std::shared_ptr<const files::FileIconImage>> overscan;
    for (std::uint64_t id = 1; id <= 16; ++id) {
      entry.id = id;
      overscan.push_back(thumbnails.request(entry, 1024, false));
      require(overscan.back() != nullptr, "overscan admission failed before its byte limit");
    }
    entry.id = 17;
    auto visible = thumbnails.request(entry, 1024, true);
    require(
        visible && thumbnails.reservedBytes() == 64U * 1024U * 1024U,
        "visible request could not reclaim overscan within the byte budget"
    );
    entry.id = 1;
    require(!thumbnails.request(entry, 1024, false), "evicted overscan retained its reservation");
    pump.until([&] { return thumbnails.activeJobs() == 2; });
    require(thumbnails.queuedJobs() == 14, "decode concurrency did not stop at two jobs");
    visible.reset();
    overscan.clear();
    thumbnails.endFrame();
    pump.drain();
    require(thumbnails.queuedJobs() == 0 && thumbnails.activeJobs() == 0, "released cells continued decoding");
    thumbnails.invalidate();
    entry = source(path, pump);
    asset = thumbnails.request(entry, 48);
    pump.until([&] { return thumbnails.activeJobs() > 0; });
    thumbnails.clearCache();
    require(!thumbnails.request(entry, 48), "clear admitted a new decode before removal finished");
    pump.until([&] { return !thumbnails.clearing() && thumbnails.activeJobs() == 0; });
    require(thumbnails.error().empty() && !asset->pixels, "clear failed or a retired worker repopulated its asset");
    const auto remote = files::Location::fromUri("sftp://example.invalid/photo.png");
    files::DirectoryEntry remoteEntry{
        .id = 99,
        .location = remote,
        .name = "photo.png",
        .displayName = "photo.png",
        .type = G_FILE_TYPE_REGULAR,
        .size = 42,
        .modifiedSeconds = 123,
        .etag = std::nullopt,
        .parentDisplayName = {},
        .contentType = "image/png"
    };
    require(!thumbnails.request(remoteEntry, 48), "local policy fetched a remote entry");
    options.policy = files::ThumbnailPolicy::ALL;
    thumbnails.setOptions(options);
    auto remoteAsset = thumbnails.request(remoteEntry, 48);
    pump.until([&] { return remoteAsset->ready; });
    require(remoteAsset->pixels.has_value(), "all-locations policy did not use an existing remote PNG cache");
    auto owner = std::make_unique<files::ThumbnailService>();
    auto retiring = owner->request(entry, 48);
    owner->setChangedCallback([&] { owner.reset(); });
    pump.until([&] { return !owner; });
    pump.drain();
    require(!retiring->pixels, "destruction retained prepared image ownership");
  }
} // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args;
  for (int i = 1; i < argc; ++i)
    args.emplace_back(argv[i]);
  if (!args.empty() && args.front() == "--thumbnail-worker")
    return files::runThumbnailWorker(std::span(args).subspan(1));
  initLogLevelFromEnvironment();
  try {
    Fixture fixture;
    if (!args.empty() && (args.front() == "--benchmark" || args.front() == "--benchmark-videos")) {
      const bool videos = args.front() == "--benchmark-videos";
      require(
          args.size() == 2 || args.size() == 3,
          videos ? "usage: thumbnail-test --benchmark-videos DIRECTORY [COUNT]"
                 : "usage: thumbnail-test --benchmark DIRECTORY [COUNT]"
      );
      benchmarkThumbnails(std::string(args[1]), args.size() == 3 ? std::stoul(std::string(args[2])) : 48, videos);
      return 0;
    }
    testCodecs();
    testRasterFormats();
    testWorkerTransport();
    testCache();
    testCacheLimits();
    testBatchAndLargerCache();
    testProviders(fixture);
    testCacheLanesAndCancellation(fixture);
    testPublicationAndSourceChanges(fixture);
    testService(fixture);
    std::println("thumbnail checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "thumbnail test: {}", error.what());
    return 1;
  }
}
