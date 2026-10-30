#include "browser/thumbnail_store.h"

#include "config/atomic_file.h"
#include "fs/gio_ptr.h"

#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  constexpr std::size_t kMaxEntries = 1024;
  std::string_view directory(int side) {
    return side <= 128 ? "normal" : side <= 256 ? "large" : side <= 512 ? "x-large" : "xx-large";
  }
  struct Fd {
    int value;
    ~Fd() {
      if (value >= 0)
        ::close(value);
    }
  };
  std::string cacheIdentity(const struct stat& info) {
    return std::format(
        "{}:{}:{}:{}:{}", info.st_dev, info.st_ino, info.st_size, info.st_mtim.tv_sec, info.st_mtim.tv_nsec
    );
  }
  bool safeDirectory(const std::filesystem::path& path) {
    if (::mkdir(path.c_str(), 0700) < 0 && errno != EEXIST)
      return false;
    const Fd directory{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
    struct stat info{};
    return directory.value >= 0
        && ::fstat(directory.value, &info) == 0
        && info.st_uid == ::getuid()
        && ::fchmod(directory.value, 0700) == 0;
  }
  bool safeName(const std::string& name) {
    const auto slash = name.find('/');
    if (slash == std::string::npos)
      return false;
    const auto prefix = name.substr(0, slash);
    const auto leaf = name.substr(slash + 1);
    return (prefix == "normal" || prefix == "large" || prefix == "x-large" || prefix == "xx-large")
        && leaf.size() == 36
        && leaf.ends_with(".png")
        && leaf.substr(0, 32).find_first_not_of("0123456789abcdef") == std::string::npos;
  }
  nlohmann::json readIndex(const std::optional<std::string>& text) {
    if (!text)
      return nlohmann::json::array();
    auto index = nlohmann::json::parse(*text);
    if (!index.is_array() || index.size() > kMaxEntries)
      throw std::runtime_error("invalid thumbnail index");
    for (const auto& item : index)
      if (!item.is_object()
          || !item.contains("name")
          || !item["name"].is_string()
          || !safeName(item["name"].get<std::string>())
          || !item.contains("bytes")
          || !item["bytes"].is_number_unsigned()
          || item["bytes"].get<std::uint64_t>() > 8U * 1024U * 1024U
          || !item.contains("identity")
          || !item["identity"].is_string())
        throw std::runtime_error("invalid thumbnail index entry");
    return index;
  }
  void removeOwned(const std::filesystem::path& root, const std::string& name, const std::string& identity) {
    if (!safeName(name))
      return;
    struct stat info{};
    if (::lstat((root / name).c_str(), &info) == 0
        && S_ISREG(info.st_mode)
        && info.st_uid == ::getuid()
        && cacheIdentity(info) == identity)
      (void)::unlink((root / name).c_str());
  }

} // namespace

files::ThumbnailStore::ThumbnailStore() {
  const char* cache = std::getenv("XDG_CACHE_HOME");
  if (cache && *cache)
    m_root = std::filesystem::path(cache) / "thumbnails";
  else {
    const char* home = std::getenv("HOME");
    if (!home || !*home)
      throw std::runtime_error("HOME is required for the thumbnail cache");
    m_root = std::filesystem::path(home) / ".cache/thumbnails";
  }
  if (!m_root.is_absolute())
    throw std::runtime_error("XDG_CACHE_HOME must be absolute");
  m_index = m_root.parent_path() / "noctalia-files/thumbnail-index.json";
}
int files::ThumbnailStore::sizeClass(int pixels) {
  return pixels <= 128 ? 128 : pixels <= 256 ? 256 : pixels <= 512 ? 512 : 1024;
}
std::string files::ThumbnailStore::filename(const std::string& uri) {
  const GString hash(g_compute_checksum_for_string(G_CHECKSUM_MD5, uri.c_str(), static_cast<gssize>(uri.size())));
  return std::string(hash.get()) + ".png";
}
std::optional<std::vector<std::uint8_t>>
files::readThumbnailFile(const std::filesystem::path& path, std::size_t maximum) {
  const Fd file{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
  struct stat info{};
  if (file.value < 0
      || ::fstat(file.value, &info) < 0
      || !S_ISREG(info.st_mode)
      || info.st_size <= 0
      || static_cast<std::uint64_t>(info.st_size) > maximum)
    return std::nullopt;
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto n = ::read(file.value, bytes.data() + offset, bytes.size() - offset);
    if (n <= 0)
      return std::nullopt;
    offset += static_cast<std::size_t>(n);
  }
  return bytes;
}
std::optional<files::ThumbnailPng> files::ThumbnailStore::read(
    const std::string& uri, int side, std::uint64_t mtime, std::optional<std::uint64_t> size, const std::string& stamp
) {
  const auto bytes = readThumbnailFile(m_root / directory(side) / filename(uri), 8U * 1024U * 1024U);
  if (!bytes)
    return std::nullopt;
  auto image = readThumbnailPng(*bytes, side);
  if (!image
      || !image->text.contains("Thumb::URI")
      || image->text.at("Thumb::URI") != uri
      || !image->text.contains("Thumb::MTime")
      || image->text.at("Thumb::MTime") != std::to_string(mtime))
    return std::nullopt;
  if (size && image->text.contains("Thumb::Size") && image->text.at("Thumb::Size") != std::to_string(*size))
    return std::nullopt;
  if (!stamp.empty()
      && image->text.contains("X-Noctalia-Files::Stamp")
      && image->text.at("X-Noctalia-Files::Stamp") != stamp)
    return std::nullopt;
  return std::move(*image);
}
void files::ThumbnailStore::write(ThumbnailPng thumbnail, int side, std::uint64_t budget, GCancellable* cancellable) {
  if (g_cancellable_is_cancelled(cancellable))
    return;
  const auto uri = thumbnail.text.at("Thumb::URI");
  const auto name = std::string(directory(side)) + '/' + filename(uri);
  thumbnail.text["Software"] = "Noctalia Files";
  auto encoded = writeThumbnailPng(thumbnail);
  if (!encoded)
    return;
  // The ledger lock serializes cache pruning/clearing across cooperating Files processes.
  std::string writtenIdentity;
  const auto outcome = updateTextFileAtomic(
      m_index, [&](const std::optional<std::string>& text) -> std::expected<std::optional<std::string>, std::string> {
        try {
          auto index = readIndex(text);
          for (auto it = index.begin(); it != index.end();) {
            if ((*it)["name"] == name)
              it = index.erase(it);
            else
              ++it;
          }
          std::uint64_t bytes = encoded->size();
          for (const auto& item : index)
            bytes += item["bytes"].get<std::uint64_t>();
          while (!index.empty() && (bytes > budget || index.size() >= kMaxEntries)) {
            bytes -= index.front()["bytes"].get<std::uint64_t>();
            removeOwned(m_root, index.front()["name"].get<std::string>(), index.front()["identity"].get<std::string>());
            index.erase(index.begin());
          }
          if (encoded->size() > budget || g_cancellable_is_cancelled(cancellable))
            return std::nullopt;
          if (!safeDirectory(m_root) || !safeDirectory(m_root / directory(side)))
            return std::unexpected("thumbnail cache directory is not private and owned");
          std::string pattern = (m_root / directory(side) / ".noctalia-XXXXXX").native();
          const Fd file{::mkstemp(pattern.data())};
          if (file.value < 0)
            return std::unexpected("cannot create thumbnail cache file");
          std::size_t offset = 0;
          while (offset < encoded->size()) {
            const auto count = ::write(file.value, encoded->data() + offset, encoded->size() - offset);
            if (count <= 0) {
              (void)::unlink(pattern.c_str());
              return std::unexpected("cannot write thumbnail cache file");
            }
            offset += static_cast<std::size_t>(count);
          }
          if (g_cancellable_is_cancelled(cancellable) || ::rename(pattern.c_str(), (m_root / name).c_str()) < 0) {
            (void)::unlink(pattern.c_str());
            return std::nullopt;
          }
          struct stat written{};
          if (::fstat(file.value, &written) < 0)
            return std::unexpected("cannot identify cached thumbnail");
          writtenIdentity = cacheIdentity(written);
          index.push_back({{"name", name}, {"bytes", encoded->size()}, {"identity", writtenIdentity}});
          return index.dump();
        } catch (const std::exception& error) {
          return std::unexpected(error.what());
        }
      }
  );
  if (outcome.state == AtomicWriteState::NOT_COMMITTED && !writtenIdentity.empty())
    removeOwned(m_root, name, writtenIdentity);
}
std::string files::ThumbnailStore::clear() { return prune(0); }
std::string files::ThumbnailStore::prune(std::uint64_t budget) {
  const auto result = updateTextFileAtomic(
      m_index, [&](const std::optional<std::string>& text) -> std::expected<std::optional<std::string>, std::string> {
        try {
          auto index = readIndex(text);
          std::uint64_t bytes = 0;
          for (const auto& item : index)
            bytes += item["bytes"].get<std::uint64_t>();
          while (!index.empty() && bytes > budget) {
            const auto& item = index.front();
            bytes -= item["bytes"].get<std::uint64_t>();
            removeOwned(m_root, item["name"].get<std::string>(), item["identity"].get<std::string>());
            index.erase(index.begin());
          }
          return index.dump();
        } catch (const std::exception& error) {
          return std::unexpected(error.what());
        }
      }
  );
  return result.error;
}
