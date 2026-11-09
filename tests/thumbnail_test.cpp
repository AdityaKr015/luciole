#include "core/log.h"
#include "event_loop/glib_poll_source.h"
#include "imaging/pixbuf_image_decoder.h"
#include "imaging/png_decoder.h"
#include "imaging/raster_decoder.h"
#include "render/core/image_orientation.h"
#include "render/core/image_resize.h"
#include "test_check.h"
#include "thumbnails/thumbnail_provider.h"
#include "thumbnails/thumbnail_service.h"
#include "thumbnails/thumbnail_store.h"
#include "thumbnails/thumbnail_worker.h"

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
  class Pump {
  public:
    void until(const std::function<bool()>& done) {
      const auto limit = std::chrono::steady_clock::now() + 20s;
      while (!done()) {
        TEST_CHECK(std::chrono::steady_clock::now() < limit);
        std::vector<pollfd> fds;
        const auto start = m_source.addPollFds(fds);
        const int requested = m_source.pollTimeoutMs();
        TEST_CHECK(::poll(fds.data(), fds.size(), requested < 0 ? 20 : std::min(requested, 20)) >= 0);
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
      TEST_CHECK(temporary != nullptr);
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
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
        image.rgba[offset] = x < width / 2 ? 240 : 20;
        image.rgba[offset + 1] = x < width / 2 ? 20 : 240;
        image.rgba[offset + 2] = 40;
        image.rgba[offset + 3] = y < height / 2 ? 128 : 255;
      }
    }
    return image;
  }
  void write(const fs::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    TEST_CHECK(static_cast<bool>(stream));
  }
  std::vector<std::uint8_t> png(const LoadedImageFile& image) {
    auto bytes = files::writeThumbnailPng({.image = image, .text = {}});
    TEST_CHECK(bytes.has_value());
    return std::move(*bytes);
  }
  files::DirectoryEntry source(const fs::path& path, Pump& pump) {
    files::DirectorySession session;
    session.open(files::Location::fromPath(path.parent_path().native()));
    pump.until([&] { return session.snapshot().status != files::DirectoryStatus::Loading; });
    TEST_CHECK(session.snapshot().status == files::DirectoryStatus::Ready);
    for (const auto& entry : session.snapshot().entries) {
      if (entry.name == path.filename().native()) {
        return entry;
      }
    }
    throw std::runtime_error("missing thumbnail fixture");
  }
  void testCodecs() {
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const auto original = picture();
    auto data = png(original);
    auto decoded = files::decodeRasterImage(data, 128, 1000000, cancel.get());
    TEST_CHECK(decoded && decoded->rgba == original.rgba && decoded->width == 32 && decoded->height == 16);
    TEST_CHECK(!files::decodeRasterImage(data, 128, 100, cancel.get()));
    auto small = files::decodeRasterImage(data, 8, 1000000, cancel.get());
    TEST_CHECK(small && small->width == 8 && small->height == 4 && small->rgba[3] >= 127 && small->rgba[3] <= 129);
    // Exercise the full-size output allowed by system loaders, independently of their scaling support.
    const GioPtr<GdkPixbuf> fullSize(gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 1600, 900));
    gdk_pixbuf_fill(fullSize.get(), 0x20408080U);
    auto fitted = files::fitRasterPixels(fullSize.get(), 512, 2'000'000);
    TEST_CHECK(
        fitted && fitted->width == 512 && fitted->height == 288 && fitted->rgba[0] == 32 && fitted->rgba[3] == 128
    );
    TEST_CHECK(!files::fitRasterPixels(fullSize.get(), 512, 1'000'000));
    const std::string svg =
        "<svg xmlns='http://www.w3.org/2000/svg'><image href='https://invalid.example/a.png'/></svg>";
    TEST_CHECK(!files::decodeRasterImage(
        {reinterpret_cast<const std::uint8_t*>(svg.data()), svg.size()}, 128, 1000000, cancel.get()
    ));
    std::uint8_t* webp = nullptr;
    const auto length =
        WebPEncodeLosslessRGBA(original.rgba.data(), original.width, original.height, original.width * 4, &webp);
    TEST_CHECK(length > 0);
    decoded = files::decodeRasterImage({webp, length}, 128, 1000000, cancel.get());
    WebPFree(webp);
    TEST_CHECK(decoded && decoded->rgba == original.rgba);
    const GioPtr<GdkPixbuf> pixbuf(gdk_pixbuf_new(GDK_COLORSPACE_RGB, false, 8, 32, 16));
    gdk_pixbuf_fill(pixbuf.get(), 0xff3020ffU);
    gchar* jpeg = nullptr;
    gsize jpegSize = 0;
    TEST_CHECK(gdk_pixbuf_save_to_buffer(pixbuf.get(), &jpeg, &jpegSize, "jpeg", nullptr, "quality", "100", nullptr));
    std::vector<std::uint8_t> oriented(
        reinterpret_cast<std::uint8_t*>(jpeg), reinterpret_cast<std::uint8_t*>(jpeg) + jpegSize
    );
    g_free(jpeg);
    const std::array<std::uint8_t, 36> exif{0xff, 0xe1, 0, 34, 'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0, 8, 0, 0, 0, 1,
                                            0,    0x12, 1, 3,  0,   1,   0,   0,   0, 6, 0,   0,   0,  0, 0, 0, 0};
    oriented.insert(oriented.begin() + 2, exif.begin(), exif.end());
    decoded = files::decodeRasterImage(oriented, 128, 1000000, cancel.get());
    TEST_CHECK(decoded && decoded->width == 16 && decoded->height == 32);
    TEST_CHECK(!files::readThumbnailPng(std::span(data).first(20), 128));
    data[20] ^= 1;
    TEST_CHECK(!files::readThumbnailPng(data, 128));
    g_cancellable_cancel(cancel.get());
    TEST_CHECK(!files::decodeRasterImage(png(original), 128, 1000000, cancel.get()));
  }
  void testRasterFormats() {
    const GioPtr<GCancellable> cancel(g_cancellable_new());
    const std::vector<std::uint8_t> gif{'G', 'I',  'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0, 0,    0, 0, 255, 255,
                                        255, 0x2c, 0,   0,   0,   0,   1, 0, 1, 0, 0,    2, 2, 0x44, 1, 0, 0x3b};
    auto decoded = files::decodeRasterImage(gif, 128, 1000000, cancel.get());
    TEST_CHECK(decoded && decoded->width == 1 && decoded->height == 1 && decoded->rgba[3] == 255);
    const std::vector<std::uint8_t> pnm{'P', '6', '\n', '2', ' ', '1', '\n', '2', '5', '5', '\n', 255, 0, 0, 0, 255, 0};
    decoded = files::decodeRasterImage(pnm, 128, 1000000, cancel.get());
    TEST_CHECK(
        decoded && decoded->width == 2 && decoded->height == 1 && decoded->rgba[0] == 255 && decoded->rgba[5] == 255
    );
    std::vector<std::uint8_t> bmp(62);
    const auto put32 = [](auto& data, std::size_t offset, std::uint32_t value) {
      for (std::size_t i = 0; i < 4; ++i) {
        data[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
      }
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
    decoded = files::decodeRasterImage(bmp, 128, 1000000, cancel.get());
    TEST_CHECK(decoded && decoded->width == 2 && decoded->rgba[0] == 255 && decoded->rgba[5] == 255);
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
    decoded = files::decodeRasterImage(ico, 128, 1000000, cancel.get());
    TEST_CHECK(decoded && decoded->rgba == picture().rgba);
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
    decoded = files::decodeRasterImage(ico, 128, 1000000, cancel.get());
    TEST_CHECK(
        decoded
        && decoded->width == 2
        && decoded->height == 1
        && decoded->rgba[0] == 255
        && decoded->rgba[3] == 128
        && decoded->rgba[5] == 255
    );
    TEST_CHECK(!files::decodeRasterImage(ico, 128, 1, cancel.get()));
    put32(ico, 26, 0x80000000U);
    TEST_CHECK(!files::decodeRasterImage(ico, 128, 1000000, cancel.get()));
    const std::array<std::uint8_t, 8> transparentRedBlue{255, 0, 0, 0, 0, 0, 255, 255};
    const auto resized = resizeToRgba(transparentRedBlue.data(), 2, 1, ResizeSourceLayout::Rgba, 1, 1);
    TEST_CHECK(resized.size() == 4 && resized[0] < 5 && resized[2] > 250 && resized[3] >= 125 && resized[3] <= 130);
    auto corrupt = pngData;
    corrupt[20] ^= 1;
    TEST_CHECK(!files::decodeRasterImage(corrupt, 128, 1000000, cancel.get()));
    TEST_CHECK(!files::decodePng(pngData, 100));
    const auto theme = files::decodePng(pngData);
    TEST_CHECK(theme && theme->pixels == picture().rgba);
    files::installPixbufImageDecoder();
    const auto providerBytes = png(picture(128, 64));
    const files::GString base64(g_base64_encode(providerBytes.data(), providerBytes.size()));
    const std::string uri = "data:image/png;base64," + std::string(base64.get());
    const auto cropped = loadImageFile(uri, 32, true);
    TEST_CHECK(cropped && cropped->width == 32 && cropped->height == 32);
    TEST_CHECK(cropped->sourceWidth == 128 && cropped->sourceHeight == 64);
    const auto bounded = loadImageFile(uri, 32, false);
    TEST_CHECK(bounded && bounded->width == 32 && bounded->height == 16);
    const auto original = decodeRasterImage(providerBytes.data(), providerBytes.size());
    TEST_CHECK(original && original->width == 128 && original->height == 64);
    TEST_CHECK(!loadImageFile("data:image/png;base64,invalid!", 32));
    TEST_CHECK(!decodeRasterImage(nullptr, 0));
  }
  void benchmarkThumbnails(const fs::path& directory, std::size_t limit, bool videos) {
    TEST_CHECK(limit >= 1 && limit <= 256);
    Pump pump;
    files::DirectorySession session;
    session.open(files::Location::fromPath(fs::absolute(directory).native()));
    pump.until([&] { return session.snapshot().status != files::DirectoryStatus::Loading; });
    TEST_CHECK(session.snapshot().status == files::DirectoryStatus::Ready);
    auto entries = session.snapshot().entries;
    std::erase_if(entries, [videos](const auto& entry) {
      return entry.type != G_FILE_TYPE_REGULAR
          || !entry.contentType
          || !entry.contentType->starts_with(videos ? "video/" : "image/")
          || entry.symlink;
    });
    std::ranges::sort(entries, {}, &files::DirectoryEntry::name);
    if (entries.size() > limit) {
      entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(limit), entries.end());
    }
    TEST_CHECK(!entries.empty());
    files::ThumbnailService service;
    const auto phase = [&](const char* name) {
      std::vector<std::shared_ptr<const files::FileIconImage>> assets;
      assets.reserve(entries.size());
      std::vector<double> elapsed(entries.size(), -1);
      const auto started = std::chrono::steady_clock::now();
      for (const auto& entry : entries) {
        assets.push_back(service.request(entry, 128));
      }
      pump.until([&] {
        bool done = true;
        const auto now = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < assets.size(); ++i) {
          TEST_CHECK(assets[i] != nullptr);
          if (!assets[i]->ready) {
            done = false;
          } else if (elapsed[i] < 0) {
            elapsed[i] = std::chrono::duration<double, std::milli>(now - started).count();
          }
        }
        return done;
      });
      TEST_CHECK(std::ranges::all_of(assets, [](const auto& image) { return static_cast<bool>(image->pixels); }));
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
    TEST_CHECK(
        result
        && result->width == 1
        && result->height == 1
        && result->rgba == std::vector<std::uint8_t>({240, 20, 40, 128})
    );
    auto malformed = valid;
    malformed[0] = 'X';
    TEST_CHECK(!files::readThumbnailWorkerResult(malformed, 128));
    malformed = valid;
    malformed.pop_back();
    TEST_CHECK(!files::readThumbnailWorkerResult(malformed, 128));
    malformed = valid;
    malformed.push_back(0);
    TEST_CHECK(!files::readThumbnailWorkerResult(malformed, 128));
    malformed = valid;
    malformed[4] = 1;
    TEST_CHECK(!files::readThumbnailWorkerResult(malformed, 128));
    TEST_CHECK(!files::readThumbnailWorkerResult(valid, 0));
    const std::string diagnostic = "NFE1GdkPixbuf: decoder unavailable";
    const auto failed = files::readThumbnailWorkerResult({diagnostic.begin(), diagnostic.end()}, 128);
    TEST_CHECK(!failed && failed.error() == diagnostic.substr(4));
    auto oversized = std::vector<std::uint8_t>(1025, 'a');
    std::copy_n(diagnostic.begin(), 4, oversized.begin());
    TEST_CHECK(files::readThumbnailWorkerResult(oversized, 128).error() != std::string(1021, 'a'));
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
    TEST_CHECK(
        cached
        && cached->image.width == 128
        && cached->image.height == 64
        && cached->image.rgba[0] == 240
        && cached->image.rgba[3] == 128
    );
    TEST_CHECK(store.read(second, 128, 123, 42, "same").has_value() && !store.read(uri, 128, 123, 42, "changed"));
    TEST_CHECK(store.clear().empty());
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
    TEST_CHECK(fs::exists(path) && files::ThumbnailStore::filename("abc") == "900150983cd24fb0d6963f7d28e17f72.png");
    const auto cached = store.read(uri, 128, 123, 42);
    TEST_CHECK(cached && cached->image.rgba == picture().rgba);
    const GioPtr<GdkPixbuf> independent(gdk_pixbuf_new_from_file(path.c_str(), nullptr));
    const char* tag = independent ? gdk_pixbuf_get_option(independent.get(), "tEXt::Thumb::URI") : nullptr;
    TEST_CHECK(tag && uri == tag);
    TEST_CHECK(!store.read(uri, 128, 124, 42) && !store.read(uri, 128, 122, 42) && !store.read(uri, 128, 123, 43));
    struct stat mode{};
    TEST_CHECK(::stat(path.c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0600);
    auto foreign = make("sftp://example.invalid/photo.png");
    auto bytes = files::writeThumbnailPng(foreign);
    TEST_CHECK(bytes.has_value());
    const auto foreignPath = store.root() / "normal" / files::ThumbnailStore::filename(foreign.text.at("Thumb::URI"));
    write(foreignPath, *bytes);
    TEST_CHECK(store.clear().empty() && !fs::exists(path) && fs::exists(foreignPath));
    store.write(make(uri), 256, 16U * 1024U * 1024U, cancel.get());
    const auto large = store.root() / "large" / files::ThumbnailStore::filename(uri);
    TEST_CHECK(fs::exists(large));
    fs::rename(large, large.string() + ".old");
    write(large, *bytes);
    TEST_CHECK(store.clear().empty() && fs::exists(large));
    auto bad = make(uri);
    bad.text.erase("Thumb::MTime");
    bytes = files::writeThumbnailPng(bad);
    TEST_CHECK(bytes.has_value());
    write(path, *bytes);
    TEST_CHECK(!store.read(uri, 128, 123, 42));
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
    TEST_CHECK(
        store.read(first, 128, 123, 42, "source-one").has_value() && !store.read(first, 128, 123, 42, "source-two")
    );
    const auto one = store.root() / "normal" / files::ThumbnailStore::filename(first);
    const auto budget = fs::file_size(one) + 32;
    store.write(make(second, "source-two"), 128, budget, cancel.get());
    const auto two = store.root() / "normal" / files::ThumbnailStore::filename(second);
    TEST_CHECK(!fs::exists(one) && fs::exists(two) && fs::file_size(two) <= budget);
    TEST_CHECK(store.clear().empty());
  }
  void command(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
      argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);
    const GioPtr<GSubprocess> process(g_subprocess_newv(
        argv.data(),
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), nullptr
    ));
    TEST_CHECK(process && g_subprocess_wait_check(process.get(), nullptr, nullptr));
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
    TEST_CHECK(largeInput >= 0);
    const GioPtr<GCancellable> limitCancel(g_cancellable_new());
    auto limited = files::ThumbnailOptions{};
    limited.maxMegapixels = 1;
    const auto tooLarge = files::renderThumbnailProvider(largeInput, "video/mp4", 128, limited, limitCancel.get());
    ::close(largeInput);
    TEST_CHECK(!tooLarge);
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
      TEST_CHECK(input >= 0);
      auto result = files::renderThumbnailProvider(input, mime, 128, {}, cancel.get());
      ::close(input);
      return result;
    };
    const auto corrupt = fixture.root / "broken.png";
    std::ofstream(corrupt, std::ios::binary) << "not an image";
    const auto failedImage = render(corrupt, "image/png");
    TEST_CHECK(
        !failedImage && failedImage.error().contains("GdkPixbuf") && !failedImage.error().contains(corrupt.native())
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
    if (!richImage) {
      std::println(stderr, "JPEG XL fixture error: {}", richImage.error());
    }
    TEST_CHECK(
        richImage
        && richImage->width == 4
        && richImage->height == 4
        && richImage->rgba == std::vector<std::uint8_t>(jxlRgba.begin(), jxlRgba.end())
    );
    const auto avif = fixture.root / "format.avif";
    command(
        {"ffmpeg", "-nostdin", "-v", "error", "-i", rasterSource.native(), "-frames:v", "1", "-c:v", "libaom-av1",
         "-cpu-used", "8", "-still-picture", "1", "-pix_fmt", "yuv444p", "-threads", "1", avif.native()}
    );
    richImage = render(avif, "image/avif");
    TEST_CHECK(richImage && richImage->width == 128 && richImage->height == 64 && richImage->rgba[0] > 180);
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
    TEST_CHECK(
        richImage
        && richImage->width == 64
        && richImage->height == 32
        && richImage->rgba[0] > 220
        && richImage->rgba[2] < 30
    );
    auto still = render(video, "video/mp4");
    TEST_CHECK(still && still->width == 128 && still->height == 64 && still->rgba.front() > 220);
    still = render(shortVideo, "video/mp4");
    TEST_CHECK(still && still->width == 128 && still->height == 64 && still->rgba[0] > 220);
    still = render(longGop, "video/mp4");
    TEST_CHECK(still && still->width == 128 && still->height == 64 && still->rgba[2] > 220);
    still = render(rotatedVideo, "video/mp4");
    TEST_CHECK(still && still->width == 64 && still->height == 128 && still->rgba[2] > 220);
    still = render(intraVideo, "video/x-msvideo");
    TEST_CHECK(still && still->width == 128 && still->height == 64 && still->rgba[2] > 220);
    still = render(coveredVideo, "video/mp4");
    TEST_CHECK(still && still->width == 128 && still->height == 64 && still->rgba[2] > 220 && still->rgba[0] < 30);
    still = render(document, "application/pdf");
    TEST_CHECK(still && still->width == 128 && still->height == 64 && still->rgba.front() > 220);
    still = render(audio, "audio/mpeg");
    TEST_CHECK(still && still->width == 128 && still->height == 64);
    TEST_CHECK(!render(plain, "audio/x-wav"));
    const std::string originalPath = std::getenv("PATH");
    ::setenv("PATH", "/noctalia-thumbnail-test-no-tools", 1);
    TEST_CHECK(!render(document, "application/pdf") && !render(video, "video/mp4"));
    ::setenv("PATH", originalPath.c_str(), 1);
    const auto bin = fixture.root / "slow-bin";
    fs::create_directory(bin);
    const files::GString sleep(g_find_program_in_path("sleep"));
    TEST_CHECK(sleep != nullptr);
    const files::GString shell(g_find_program_in_path("sh"));
    TEST_CHECK(shell != nullptr);
    std::ofstream(bin / "pdftoppm") << "#!" << shell.get() << "\nexec " << sleep.get() << " 30\n";
    TEST_CHECK(::chmod((bin / "pdftoppm").c_str(), 0700) == 0);
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
    TEST_CHECK(timeoutStarted - started < 2s);
    const auto elapsed = std::chrono::steady_clock::now() - timeoutStarted;
    TEST_CHECK(!still && elapsed >= 7s && elapsed < 12s);
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
    TEST_CHECK(shell && sleep);
    std::ofstream(bin / "pdftoppm") << "#!" << shell.get() << "\nexec " << sleep.get() << " 30\n";
    TEST_CHECK(::chmod((bin / "pdftoppm").c_str(), 0700) == 0);
    const std::string path = std::getenv("PATH");
    ::setenv("PATH", bin.c_str(), 1);
    files::ThumbnailService service;
    auto a = service.request(first, 128), b = service.request(second, 128);
    TEST_CHECK(a && b);
    pump.until([&] { return service.activeJobs() == 2; });
    auto cached = service.request(warm, 128);
    TEST_CHECK(cached != nullptr);
    pump.until([&] { return cached->ready; });
    TEST_CHECK(cached->pixels && service.activeJobs() == 2 && !a->ready && !b->ready);
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
      for (std::size_t offset = 0; offset < changed.rgba.size(); offset += 4) {
        changed.rgba[offset] = static_cast<std::uint8_t>(value);
      }
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
    TEST_CHECK(!alternate.empty());
    write(path, original);
    auto entry = source(path, pump);
    files::ThumbnailService service;
    auto asset = service.request(entry, 128);
    TEST_CHECK(asset != nullptr);
    pump.until([&] { return asset->ready; });
    const auto cachePath =
        files::ThumbnailStore{}.root() / "normal" / files::ThumbnailStore::filename(entry.location.uri());
    TEST_CHECK(asset->pixels && !fs::exists(cachePath));
    pump.until([&] { return fs::exists(cachePath); });
    files::DirectorySnapshot snapshot;
    snapshot.generation = 100;
    snapshot.location = files::Location::fromPath(fixture.root.native());
    snapshot.status = files::DirectoryStatus::Ready;
    snapshot.entries.push_back(entry);
    service.synchronizeSources(snapshot);
    ++snapshot.generation;
    service.synchronizeSources(snapshot);
    TEST_CHECK(service.touch(asset, true) && service.request(entry, 128) == asset && asset->pixels);
    const auto oldMtime = fs::last_write_time(path);
    write(path, alternate);
    fs::last_write_time(path, oldMtime);
    auto updated = source(path, pump);
    TEST_CHECK(
        updated.size == entry.size
        && updated.modifiedSeconds == entry.modifiedSeconds
        && updated.contentRevision != entry.contentRevision
    );
    snapshot.entries = {updated};
    ++snapshot.generation;
    service.synchronizeSources(snapshot);
    TEST_CHECK(!service.touch(asset, true) && !asset->pixels);
    auto replacement = service.request(updated, 128);
    pump.until([&] { return replacement->ready; });
    TEST_CHECK(replacement != asset && replacement->pixels && replacement->pixels->rgba[0] == color);
  }
  void testService(const Fixture& fixture) {
    Pump pump;
    const auto path = fixture.root / "café\nimage \xff.png";
    write(path, png(picture()));
    auto entry = source(path, pump);
    files::ThumbnailService thumbnails;
    auto asset = thumbnails.request(entry, 48);
    TEST_CHECK(asset != nullptr);
    pump.until([&] { return asset->ready; });
    TEST_CHECK(asset->pixels && asset->pixels->width == 32 && asset->pixels->rgba[3] == 128);
    TEST_CHECK(thumbnails.activeJobs() <= 2 && thumbnails.reservedBytes() <= 64U * 1024U * 1024U);
    const auto original = asset;
    const auto stampBefore = fs::last_write_time(path);
    auto replacement = picture();
    std::ranges::fill(replacement.rgba, 255);
    write(path, png(replacement));
    fs::last_write_time(path, stampBefore);
    thumbnails.invalidate();
    TEST_CHECK(!original->pixels);
    entry = source(path, pump);
    asset = thumbnails.request(entry, 48);
    pump.until([&] { return asset->ready; });
    TEST_CHECK(asset->pixels && asset->pixels->rgba.front() == 255);
    auto options = files::ThumbnailOptions{};
    options.policy = files::ThumbnailPolicy::Never;
    thumbnails.setOptions(options);
    TEST_CHECK(!thumbnails.request(entry, 48));
    options.policy = files::ThumbnailPolicy::Local;
    thumbnails.setOptions(options);
    std::vector<std::shared_ptr<const files::FileIconImage>> held;
    for (std::uint64_t id = 1; id <= 1200; ++id) {
      entry.id = id;
      if (auto image = thumbnails.request(entry, 48)) {
        held.push_back(std::move(image));
      }
    }
    TEST_CHECK(
        thumbnails.reservedBytes() <= 64U * 1024U * 1024U && held.size() < 1200 && thumbnails.queuedJobs() <= 1024
    );
    thumbnails.invalidate();
    held.clear();
    std::vector<std::shared_ptr<const files::FileIconImage>> overscan;
    for (std::uint64_t id = 1; id <= 16; ++id) {
      entry.id = id;
      overscan.push_back(thumbnails.request(entry, 1024, false));
      TEST_CHECK(overscan.back() != nullptr);
    }
    entry.id = 17;
    auto visible = thumbnails.request(entry, 1024, true);
    TEST_CHECK(visible && thumbnails.reservedBytes() == 64U * 1024U * 1024U);
    entry.id = 1;
    TEST_CHECK(!thumbnails.request(entry, 1024, false));
    pump.until([&] { return thumbnails.activeJobs() == 2; });
    TEST_CHECK(thumbnails.queuedJobs() == 14);
    visible.reset();
    overscan.clear();
    thumbnails.endFrame();
    pump.drain();
    TEST_CHECK(thumbnails.queuedJobs() == 0 && thumbnails.activeJobs() == 0);
    thumbnails.invalidate();
    entry = source(path, pump);
    asset = thumbnails.request(entry, 48);
    pump.until([&] { return thumbnails.activeJobs() > 0; });
    thumbnails.clearCache();
    TEST_CHECK(!thumbnails.request(entry, 48));
    pump.until([&] { return !thumbnails.clearing() && thumbnails.activeJobs() == 0; });
    TEST_CHECK(thumbnails.error().empty() && !asset->pixels);
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
    TEST_CHECK(!thumbnails.request(remoteEntry, 48));
    options.policy = files::ThumbnailPolicy::All;
    thumbnails.setOptions(options);
    auto remoteAsset = thumbnails.request(remoteEntry, 48);
    pump.until([&] { return remoteAsset->ready; });
    TEST_CHECK(static_cast<bool>(remoteAsset->pixels));
    auto owner = std::make_unique<files::ThumbnailService>();
    auto retiring = owner->request(entry, 48);
    owner->setChangedCallback([&] { owner.reset(); });
    pump.until([&] { return !owner; });
    pump.drain();
    TEST_CHECK(!retiring->pixels);
  }
} // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args;
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  if (!args.empty() && args.front() == "--thumbnail-worker") {
    return files::runThumbnailWorker(std::span(args).subspan(1));
  }
  initLogLevelFromEnvironment();
  try {
    Fixture fixture;
    if (!args.empty() && (args.front() == "--benchmark" || args.front() == "--benchmark-videos")) {
      const bool videos = args.front() == "--benchmark-videos";
      TEST_CHECK(args.size() == 2 || args.size() == 3);
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
