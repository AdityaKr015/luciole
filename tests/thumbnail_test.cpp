#include "app/glib_poll_source.h"
#include "browser/thumbnail_provider.h"
#include "browser/thumbnail_service.h"
#include "browser/thumbnail_store.h"
#include "browser/thumbnail_worker.h"
#include "render/core/image_orientation.h"

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
        small && small->width == 8 && small->height == 4 && small->rgba[3] == 128,
        "downscaling lost aspect or transparency"
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
    auto still = render(video, "video/mp4");
    require(
        still && still->width == 128 && still->height == 64 && still->rgba.front() > 220,
        "video provider did not render the expected still"
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
        .parentDisplayName = {}
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
  try {
    Fixture fixture;
    testCodecs();
    testCache();
    testCacheLimits();
    testProviders(fixture);
    testService(fixture);
    std::println("thumbnail checks passed");
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "thumbnail test: {}", error.what());
    return 1;
  }
}
