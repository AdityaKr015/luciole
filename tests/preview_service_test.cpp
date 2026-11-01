#include "browser/preview_layout.h"
#include "browser/preview_service.h"
#include "browser/thumbnail_worker.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <print>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>

namespace {
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  void wait(auto predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("preview timed out");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  files::DirectoryEntry entry(const std::filesystem::path& path, std::string mime = "text/plain") {
    files::DirectoryEntry result{
        .id = 1,
        .location = files::Location::fromPath(path.native()),
        .name = path.filename().native(),
        .displayName = path.filename().native(),
        .size = std::nullopt,
        .modifiedSeconds = std::nullopt,
        .etag = std::nullopt,
        .parentDisplayName = {}
    };
    result.id = 1;
    result.location = files::Location::fromPath(path.native());
    result.name = path.filename().native();
    result.displayName = result.name;
    result.type = G_FILE_TYPE_REGULAR;
    result.contentType = std::move(mime);
    struct stat stat{};
    if (::lstat(path.c_str(), &stat) == 0) {
      result.size = static_cast<std::uint64_t>(stat.st_size);
      result.modifiedSeconds = static_cast<std::uint64_t>(stat.st_mtim.tv_sec);
    }
    return result;
  }
  void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
  }
  void checks(const std::filesystem::path& root) {
    const auto portraitLayout = files::previewImageLayout(1000, 3000, 320, 1300, 1);
    require(
        portraitLayout.pixels >= 846, "tall portrait was decoded for the pane width instead of its displayed height"
    );
    const auto landscapeLayout = files::previewImageLayout(3840, 2160, 320, 1300, 1);
    require(landscapeLayout.pixels <= 320, "landscape preview unnecessarily decoded for the entire pane height");
    const auto retinaLayout = files::previewImageLayout(3840, 2160, 800, 720, 2);
    require(retinaLayout.pixels > 1024 && retinaLayout.pixels <= 2048, "high-DPI preview target lost pixel density");
    files::PreviewService preview;
    const auto a = root / "space ' café\n.txt", b = root / "new.txt";
    const auto finish = [&] { wait([&] { return !preview.busy(); }); };
    write(a, "<b>literal</b>\n日本語\n");
    write(b, "current file");
    preview.show(entry(a), 512, {});
    finish();
    require(
        preview.snapshot().status == files::PreviewStatus::READY
            && preview.snapshot().text == "<b>literal</b>\n日本語\n",
        "literal Unicode text was changed"
    );
    for (int i = 0; i < 100; ++i)
      preview.show(entry(i % 2 ? a : b), 512, {});
    preview.show(entry(b), 512, {});
    finish();
    require(
        preview.snapshot().text == "current file" && preview.snapshot().entry->location == entry(b).location,
        "superseded preview published"
    );
    write(a, std::string(1000000, 'a'));
    preview.show(entry(a), 512, {});
    finish();
    require(
        preview.snapshot().status == files::PreviewStatus::READY
            && preview.snapshot().text.size() < 9000
            && preview.snapshot().message == "Preview truncated",
        "long-line preview was not bounded"
    );
    write(b, std::string("binary\0text", 11));
    preview.show(entry(b), 512, {});
    finish();
    require(preview.snapshot().status == files::PreviewStatus::UNAVAILABLE, "binary text accepted");
    auto stale = entry(a);
    stale.size = 3;
    preview.show(stale, 512, {});
    finish();
    require(preview.snapshot().status == files::PreviewStatus::ERROR, "stale file revision accepted");
    const auto link = root / "link.txt";
    std::filesystem::create_symlink(a, link);
    preview.show(entry(link), 512, {});
    finish();
    require(preview.snapshot().text.empty(), "symbolic link followed");
    const auto fifo = root / "fifo.txt";
    require(::mkfifo(fifo.c_str(), 0600) == 0, "FIFO fixture failed");
    preview.show(entry(fifo), 512, {});
    finish();
    require(preview.snapshot().status == files::PreviewStatus::UNAVAILABLE, "special file read");
    auto remote = entry(b);
    remote.location = files::Location::fromUri("sftp://example.invalid/file.txt");
    preview.show(remote, 512, {});
    finish();
    require(preview.snapshot().status == files::PreviewStatus::UNAVAILABLE, "remote text was downloaded");
    preview.show(entry(a), 512, {});
    preview.clear();
    finish();
    require(
        preview.snapshot().status == files::PreviewStatus::EMPTY
            && !preview.snapshot().entry
            && preview.snapshot().text.empty(),
        "hidden preview republished retired text"
    );
    const auto corrupt = root / "corrupt.png";
    write(corrupt, "not an image");
    auto image = entry(corrupt, "image/png");
    preview.show(image, 512, {});
    wait([&] { return preview.snapshot().status != files::PreviewStatus::LOADING; });
    require(preview.snapshot().status == files::PreviewStatus::UNAVAILABLE, "corrupt image did not settle");
    preview.clear();
    finish();
    const auto good = root / "good.png";
    const files::GioPtr<GdkPixbuf> pixels(gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 8, 8));
    gdk_pixbuf_fill(pixels.get(), 0xff2244ffU);
    require(gdk_pixbuf_save(pixels.get(), good.c_str(), "png", nullptr, nullptr), "PNG fixture failed");
    preview.show(entry(good, "image/png"), 1024, {});
    wait([&] { return preview.snapshot().status != files::PreviewStatus::LOADING; });
    require(
        preview.snapshot().status == files::PreviewStatus::READY
            && preview.snapshot().image
            && preview.snapshot().image->pixels,
        "real image preview did not decode"
    );
    preview.clear();
    finish();
    files::ThumbnailService grid;
    const auto source = entry(good, "image/png");
    const auto loaded = grid.request(source, 128);
    wait([&] { return loaded && loaded->ready; });
    const auto seed = grid.prepared(source);
    require(seed && seed->pixels, "loaded browser thumbnail not reusable");
    preview.show(source, 1024, {}, seed);
    require(
        preview.snapshot().status == files::PreviewStatus::READY && preview.snapshot().image == seed,
        "loaded browser image was replaced by an intermediate icon"
    );
    auto obsolete = source;
    obsolete.contentRevision = "changed";
    require(!grid.prepared(obsolete), "stale browser thumbnail reused for a new revision");
    preview.clear();
    finish();
    const auto detailed = root / "detailed.png";
    const files::GioPtr<GdkPixbuf> detailPixels(gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 2560, 1600));
    gdk_pixbuf_fill(detailPixels.get(), 0x2233ffffU);
    require(gdk_pixbuf_save(detailPixels.get(), detailed.c_str(), "png", nullptr, nullptr), "large PNG fixture failed");
    const auto detailSource = entry(detailed, "image/png");
    const auto small = grid.request(detailSource, 128);
    wait([&] { return small && small->ready; });
    require(small->pixels && small->pixels->width == 128, "small thumbnail fixture was not small");
    preview.show(detailSource, 512, {}, small);
    require(preview.snapshot().image == small, "preview lost its immediate thumbnail");
    wait([&] {
      return preview.snapshot().image
          && preview.snapshot().image->pixels
          && preview.snapshot().image->pixels->width >= 512;
    });
    preview.show(detailSource, 1024, {}, small);
    finish();
    require(
        preview.snapshot().image && preview.snapshot().image->pixels && preview.snapshot().image->pixels->width == 1024,
        "resized preview kept the small thumbnail"
    );
    require(!preview.snapshot().imageUpgradeFailed, "successful preview reported an upgrade failure");
    preview.show(detailSource, 1536, {}, small);
    finish();
    require(
        preview.snapshot().image && preview.snapshot().image->pixels && preview.snapshot().image->pixels->width == 2048,
        "high-DPI preview was limited to a grid thumbnail"
    );
    const auto highResolution = preview.snapshot().image;
    preview.show(detailSource, 1400, {}, small);
    require(preview.snapshot().image == highResolution, "resize within one pixel class restarted the preview");
    preview.show(detailSource, 512, {}, small);
    require(preview.snapshot().image == highResolution, "narrowing the pane downgraded its existing image");
    const auto ordinary = grid.request(detailSource, 1536);
    wait([&] { return ordinary && ordinary->ready; });
    require(ordinary->pixels && ordinary->pixels->width == 1024, "grid thumbnail bounds were enlarged");
    preview.clear();
    finish();
    const auto limitedPath = root / "limited.png";
    std::filesystem::copy_file(detailed, limitedPath);
    const auto limitedSource = entry(limitedPath, "image/png");
    const auto fallback = grid.request(limitedSource, 128);
    wait([&] { return fallback && fallback->ready; });
    files::ThumbnailOptions limited;
    limited.maxMegapixels = 1;
    preview.show(limitedSource, 512, limited, fallback);
    finish();
    require(
        preview.snapshot().status == files::PreviewStatus::READY
            && preview.snapshot().image == fallback
            && preview.snapshot().image->pixels
            && preview.snapshot().image->pixels->width == 128
            && preview.snapshot().message == "Larger preview unavailable."
            && preview.snapshot().imageUpgradeFailed,
        "failed larger preview lost its usable thumbnail"
    );
    preview.clear();
    finish();
    {
      files::PreviewService retired;
      retired.show(entry(a), 512, {});
    }
    write(b, "latest revision");
    preview.show(entry(b), 512, {});
    finish();
    require(preview.snapshot().text == "latest revision", "another owner interfered with preview");
  }
} // namespace
int main(int argc, char** argv) {
  std::vector<std::string_view> arguments;
  for (int i = 1; i < argc; ++i)
    arguments.emplace_back(argv[i]);
  if (!arguments.empty() && arguments.front() == "--thumbnail-worker")
    return files::runThumbnailWorker(std::span(arguments).subspan(1));
  const files::GString directory(g_dir_make_tmp("noctalia-preview-test-XXXXXX", nullptr));
  if (!directory)
    return 1;
  const std::filesystem::path root(directory.get());
  g_setenv("XDG_CACHE_HOME", (root / "cache").c_str(), true);
  int status = 0;
  try {
    checks(root);
    std::println("preview checks passed");
  } catch (const std::exception& error) {
    std::println(stderr, "preview: {}", error.what());
    status = 1;
  }
  std::filesystem::remove_all(root);
  return status;
}
