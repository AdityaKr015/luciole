#include "operations/archive_work.h"

#include "fs/filename.h"
#include "fs/gio_ptr.h"

#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cerrno>
#include <clocale>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
  using namespace files;
  namespace fs = std::filesystem;
  using Clock = std::chrono::steady_clock;
  class ArchiveLocale {
  public:
    ArchiveLocale() {
      m_locale = ::newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
      if (!m_locale) {
        throw std::runtime_error("A UTF-8 archive locale is unavailable.");
      }
      m_previous = ::uselocale(m_locale);
    }
    ~ArchiveLocale() {
      ::uselocale(m_previous);
      ::freelocale(m_locale);
    }
    ArchiveLocale(const ArchiveLocale&) = delete;
    ArchiveLocale& operator=(const ArchiveLocale&) = delete;

  private:
    locale_t m_locale{}, m_previous{};
  };
  class Fd {
  public:
    explicit Fd(int value = -1) : m_value(value) {}
    ~Fd() {
      if (m_value >= 0) {
        ::close(m_value);
      }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : m_value(std::exchange(other.m_value, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
      if (this != &other) {
        if (m_value >= 0) {
          ::close(m_value);
        }
        m_value = std::exchange(other.m_value, -1);
      }
      return *this;
    }
    int get() const { return m_value; }

  private:
    int m_value;
  };
  [[noreturn]] void systemError(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
  }
  Fd opened(int fd) {
    if (fd < 0) {
      systemError("Could not open archive item");
    }
    return Fd(fd);
  }
  struct stat statFd(int fd) {
    struct stat value{};
    if (::fstat(fd, &value) != 0) {
      systemError("Could not inspect archive item");
    }
    return value;
  }
  struct stat statAt(int fd, const std::string& name) {
    struct stat value{};
    if (::fstatat(fd, name.c_str(), &value, AT_SYMLINK_NOFOLLOW) != 0) {
      systemError("Could not inspect archive source");
    }
    return value;
  }
  std::string stamp(const struct stat& info) {
    return std::format(
        "{}:{}:{}:{}:{}:{}:{}:{}", info.st_dev, info.st_ino, info.st_mode, info.st_size, info.st_mtim.tv_sec,
        info.st_mtim.tv_nsec, info.st_ctim.tv_sec, info.st_ctim.tv_nsec
    );
  }
  struct Work {
    ArchiveLimits limits;
    GCancellable* cancel;
    files::archive_work::Progress& progress;
    Clock::time_point deadline;
    std::exception_ptr failure;
    std::uint64_t expanded = 0;
    std::size_t count = 0;
    void check() const {
      if (g_cancellable_is_cancelled(cancel)) {
        throw std::runtime_error("Archive operation cancelled.");
      }
      if (Clock::now() > deadline) {
        throw std::runtime_error("Archive operation exceeded its time limit.");
      }
    }
    void item() {
      check();
      if (++count > limits.maxEntries) {
        throw std::runtime_error("Archive has too many entries.");
      }
      progress.items.store(count);
    }
    void account(std::uint64_t bytes) {
      if (bytes > limits.maxExpandedBytes - expanded) {
        throw std::runtime_error("Archive exceeds the expanded-size limit.");
      }
      expanded += bytes;
    }
  };
  Work workFor(ArchiveLimits limits, GCancellable* cancel, files::archive_work::Progress& progress) {
    return {limits, cancel, progress, Clock::now() + limits.maxTime, {}, 0, 0};
  }
  std::string bounded(const char* text) {
    if (!text) {
      throw std::runtime_error("Archive entry has no pathname.");
    }
    const auto size = ::strnlen(text, 4097);
    if (size > 4096) {
      throw std::runtime_error("Archive pathname is too long.");
    }
    return {text, size};
  }
  bool absolute(const std::string& name) {
    return !name.empty()
        && (name.front() == '/'
            || name.front() == '\\'
            || (name.size() >= 2 && g_ascii_isalpha(name[0]) && name[1] == ':'));
  }
  std::vector<std::string> parts(const std::string& name) {
    if (absolute(name) || name.contains('\\')) {
      throw std::runtime_error("Archive contains an absolute or nonportable pathname.");
    }
    std::vector<std::string> result;
    for (std::size_t start = 0; start < name.size();) {
      const auto end = name.find('/', start);
      auto component = name.substr(start, end == std::string::npos ? end : end - start);
      if (component == "..") {
        throw std::runtime_error("Archive contains a parent-path traversal.");
      }
      if (!component.empty() && component != ".") {
        result.push_back(std::move(component));
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    return result;
  }
  std::string join(const std::vector<std::string>& components) {
    std::string result;
    for (const auto& component : components) {
      if (!result.empty()) {
        result += '/';
      }
      result += component;
    }
    return result;
  }
  std::string safePath(const std::string& name, const Work& work) {
    auto components = parts(name);
    if (components.size() > work.limits.maxDepth) {
      throw std::runtime_error("Archive paths are too deep.");
    }
    return join(components);
  }
  std::string linkTarget(const std::string& path, const std::string& target, const Work& work) {
    if (target.empty() || absolute(target) || target.contains('\\')) {
      throw std::runtime_error("Archive link escapes the extraction folder.");
    }
    auto resolved = parts(path);
    resolved.pop_back();
    for (std::size_t start = 0; start < target.size();) {
      const auto end = target.find('/', start);
      const auto component = target.substr(start, end == std::string::npos ? end : end - start);
      if (component == "..") {
        if (resolved.empty()) {
          throw std::runtime_error("Archive link escapes the extraction folder.");
        }
        resolved.pop_back();
      } else if (!component.empty() && component != ".") {
        resolved.push_back(component);
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    if (resolved.size() > work.limits.maxDepth) {
      throw std::runtime_error("Archive link is too deep.");
    }
    return join(resolved);
  }
  void validateLinks(const std::map<std::string, std::string>& links, Work& work) {
    for (const auto& [path, target] : links) {
      auto resolved = parts(path);
      resolved.pop_back();
      std::deque<std::string> remaining;
      const auto append = [&](const std::string& text) {
        if (absolute(text) || text.contains('\\')) {
          throw std::runtime_error("Archive link escapes the extraction folder.");
        }
        std::vector<std::string> tokens;
        for (std::size_t start = 0; start < text.size();) {
          const auto end = text.find('/', start);
          auto token = text.substr(start, end == std::string::npos ? end : end - start);
          if (!token.empty() && token != ".") {
            tokens.push_back(std::move(token));
          }
          if (end == std::string::npos) {
            break;
          }
          start = end + 1;
        }
        for (const auto& token : std::views::reverse(tokens)) {
          remaining.push_front(token);
        }
      };
      append(target);
      unsigned hops = 0;
      while (!remaining.empty()) {
        work.check();
        auto token = std::move(remaining.front());
        remaining.pop_front();
        if (token == "..") {
          if (resolved.empty()) {
            throw std::runtime_error("Archive link escapes through another link.");
          }
          resolved.pop_back();
          continue;
        }
        resolved.push_back(std::move(token));
        if (resolved.size() > work.limits.maxDepth) {
          throw std::runtime_error("Archive link is too deep.");
        }
        if (const auto found = links.find(join(resolved)); found != links.end()) {
          if (++hops > 40) {
            throw std::runtime_error("Archive has a cyclic link chain.");
          }
          resolved.pop_back();
          append(found->second);
        }
      }
    }
  }
  std::string leaf(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }
  std::string parentName(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
  }
  std::string native(const Location& location) {
    const auto value = location.nativePath();
    if (!value) {
      throw std::runtime_error("Archives currently support native files and folders only.");
    }
    return *value;
  }
  std::pair<std::string, std::string> destinationParts(const Location& destination) {
    const fs::path path(native(destination));
    const auto name = path.filename().native();
    if (const auto problem = filenameProblem(name)) {
      throw std::runtime_error(*problem);
    }
    const Fd parent = opened(::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    return {path.parent_path().native(), name};
  }
  struct Stage {
    std::string path;
    Stage() {
      const files::GString temporary(g_dir_make_tmp("noctalia-files-archive-XXXXXX", nullptr));
      if (!temporary) {
        throw std::runtime_error("Could not create private archive staging storage.");
      }
      path = temporary.get();
    }
    ~Stage() {
      if (!path.empty()) {
        std::error_code ignored;
        fs::remove_all(path, ignored);
      }
    }
    std::string release() { return std::exchange(path, {}); }
  };
  [[noreturn]] void archiveError(struct archive* value, Work& work) {
    if (work.failure) {
      std::rethrow_exception(work.failure);
    }
    throw std::runtime_error(archive_error_string(value) ? archive_error_string(value) : "Archive backend failed.");
  }
  void requireArchive(int status, struct archive* value, Work& work) {
    if (status != ARCHIVE_OK) {
      archiveError(value, work);
    }
  }
  struct Reader {
    Work& work;
    Fd input;
    std::string filename, original;
    std::unique_ptr<struct archive, decltype(&archive_read_free)> value{archive_read_new(), &archive_read_free};
    std::array<char, 65536> buffer{};
    std::uint64_t readBytes = 0;
    Reader(const Location& source, Work& request)
        : work(request), input(opened(::open(native(source).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK))),
          filename(native(source)) {
      const auto info = statFd(input.get());
      if (!S_ISREG(info.st_mode)
          || info.st_size <= 0
          || static_cast<std::uint64_t>(info.st_size) > work.limits.maxArchiveBytes) {
        throw std::runtime_error("Archive is empty, special, or exceeds the input-size limit.");
      }
      original = stamp(info);
      if (!value) {
        throw std::runtime_error("Could not initialize the archive reader.");
      }
      requireArchive(archive_read_support_format_zip(value.get()), value.get(), work);
      requireArchive(archive_read_support_format_tar(value.get()), value.get(), work);
      requireArchive(archive_read_support_filter_none(value.get()), value.get(), work);
      if (archive_zlib_version()) {
        requireArchive(archive_read_support_filter_gzip(value.get()), value.get(), work);
      }
      requireArchive(
          archive_read_set_seek_callback(
              value.get(),
              [](struct archive* a, void* data, la_int64_t offset, int whence) -> la_int64_t {
                auto& reader = *static_cast<Reader*>(data);
                try {
                  reader.work.check();
                  const auto position = ::lseek(reader.input.get(), offset, whence);
                  if (position < 0) {
                    systemError("Could not seek archive");
                  }
                  return position;
                } catch (...) {
                  reader.work.failure = std::current_exception();
                  archive_set_error(a, EIO, "Archive seek failed");
                  return ARCHIVE_FATAL;
                }
              }
          ),
          value.get(), work
      );
      requireArchive(
          archive_read_open2(
              value.get(), this, nullptr,
              [](struct archive* a, void* data, const void** block) -> la_ssize_t {
                auto& reader = *static_cast<Reader*>(data);
                try {
                  reader.work.check();
                  if (reader.readBytes > reader.work.limits.maxArchiveBytes * 4) {
                    throw std::runtime_error("Archive exceeded its input-work limit.");
                  }
                  ssize_t count;
                  do {
                    count = ::read(reader.input.get(), reader.buffer.data(), reader.buffer.size());
                  } while (count < 0 && errno == EINTR);
                  if (count < 0) {
                    systemError("Could not read archive");
                  }
                  reader.readBytes += static_cast<std::uint64_t>(count);
                  *block = reader.buffer.data();
                  return count;
                } catch (...) {
                  reader.work.failure = std::current_exception();
                  archive_set_error(a, EIO, "Archive read failed");
                  return -1;
                }
              },
              nullptr, nullptr
          ),
          value.get(), work
      );
    }
    void unchanged() const {
      struct stat named{};
      if (stamp(statFd(input.get())) != original
          || ::lstat(filename.c_str(), &named) != 0
          || stamp(named) != original) {
        throw std::runtime_error("The archive changed during this operation. Reload its contents.");
      }
    }
    bool next(struct archive_entry*& entry) {
      work.check();
      const auto status = archive_read_next_header(value.get(), &entry);
      if (status == ARCHIVE_EOF) {
        return false;
      }
      requireArchive(status, value.get(), work);
      if (archive_entry_is_encrypted(entry) > 0) {
        throw std::runtime_error("Encrypted archives are not supported yet.");
      }
      return true;
    }
    void skip() { requireArchive(archive_read_data_skip(value.get()), value.get(), work); }
  };
  std::shared_ptr<ArchiveCatalog> catalog(Reader& reader) {
    auto result = std::make_shared<ArchiveCatalog>(
        ArchiveCatalog{Location::fromPath(reader.filename), reader.original, {}, {}, 0}
    );
    std::map<std::string, std::size_t> known;
    std::set<std::string> explicitNames;
    std::size_t metadata = 0;
    const auto add = [&](ArchiveEntry entry) {
      metadata += entry.path.size() + entry.link.size() + entry.name.size();
      if (metadata > reader.work.limits.maxMetadataBytes || result->entries.size() >= reader.work.limits.maxEntries) {
        throw std::runtime_error("Archive contents exceed the metadata limit.");
      }
      const files::GString display(g_filename_display_name(entry.name.c_str()));
      entry.displayName = display.get();
      if (entry.kind == ArchiveEntryKind::DIRECTORY) {
        entry.icon.names = {"folder"};
      } else {
        const files::GString mime(g_content_type_guess(entry.name.c_str(), nullptr, 0, nullptr));
        const files::GioPtr<GIcon> icon(mime ? g_content_type_get_icon(mime.get()) : nullptr);
        if (icon && G_IS_THEMED_ICON(icon.get())) {
          const auto* names = g_themed_icon_get_names(G_THEMED_ICON(icon.get()));
          if (names) {
            for (std::size_t i = 0; names[i]; ++i) {
              entry.icon.names.emplace_back(names[i]);
            }
          }
        }
        if (entry.icon.names.empty()) {
          entry.icon.names = {"text-x-generic"};
        }
      }
      known.emplace(entry.path, result->entries.size());
      result->entries.push_back(std::move(entry));
    };
    struct archive_entry* header = nullptr;
    while (reader.next(header)) {
      reader.work.item();
      const auto path = safePath(bounded(archive_entry_pathname(header)), reader.work);
      if (path.empty()) {
        if (archive_entry_filetype(header) != AE_IFDIR) {
          throw std::runtime_error("Archive has an empty file name.");
        }
        reader.skip();
        continue;
      }
      ArchiveEntry entry{
          .path = path,
          .parent = parentName(path),
          .name = leaf(path),
          .displayName = {},
          .size = std::nullopt,
          .link = {}
      };
      const auto type = archive_entry_filetype(header);
      if (const char* target = archive_entry_hardlink(header)) {
        entry.kind = ArchiveEntryKind::HARDLINK;
        entry.link = safePath(bounded(target), reader.work);
        if (entry.link.empty()) {
          throw std::runtime_error("Archive has an invalid hard-link target.");
        }
      } else if (type == AE_IFREG) {
        entry.kind = ArchiveEntryKind::FILE;
      } else if (type == AE_IFDIR) {
        entry.kind = ArchiveEntryKind::DIRECTORY;
      } else if (type == AE_IFLNK) {
        entry.kind = ArchiveEntryKind::SYMLINK;
        entry.link = bounded(archive_entry_symlink(header));
        (void)linkTarget(path, entry.link, reader.work);
      } else {
        throw std::runtime_error("Archive contains an unsupported special file.");
      }
      entry.mode = static_cast<unsigned>(archive_entry_perm(header)) & 0777U;
      if (entry.kind == ArchiveEntryKind::FILE && archive_entry_size_is_set(header)) {
        const auto size = archive_entry_size(header);
        if (size < 0) {
          throw std::runtime_error("Archive contains a negative file size.");
        }
        entry.size = static_cast<std::uint64_t>(size);
        reader.work.account(*entry.size);
      }
      if (explicitNames.contains(path) && entry.kind != ArchiveEntryKind::DIRECTORY) {
        throw std::runtime_error("Archive contains duplicate file names.");
      }
      explicitNames.insert(path);
      auto components = parts(entry.parent);
      std::string prefix;
      for (const auto& component : components) {
        if (!prefix.empty()) {
          prefix += '/';
        }
        prefix += component;
        const auto found = known.find(prefix);
        if (found != known.end() && result->entries[found->second].kind != ArchiveEntryKind::DIRECTORY) {
          throw std::runtime_error("Archive entry traverses a file or link.");
        }
        if (found == known.end()) {
          add(
              {.path = prefix,
               .parent = parentName(prefix),
               .name = component,
               .displayName = {},
               .kind = ArchiveEntryKind::DIRECTORY,
               .size = std::nullopt,
               .link = {}}
          );
        }
      }
      if (const auto found = known.find(path); found != known.end()) {
        if (entry.kind != ArchiveEntryKind::DIRECTORY
            || result->entries[found->second].kind != ArchiveEntryKind::DIRECTORY) {
          throw std::runtime_error("Archive entries have conflicting types.");
        }
        result->entries[found->second].mode = entry.mode;
      } else {
        add(std::move(entry));
      }
      reader.skip();
    }
    std::map<std::string, std::string> links;
    for (const auto& entry : result->entries) {
      if (entry.kind == ArchiveEntryKind::SYMLINK) {
        links.emplace(entry.path, entry.link);
      }
    }
    validateLinks(links, reader.work);
    result->expandedBytes = reader.work.expanded;
    result->format = archive_format_name(reader.value.get()) ? archive_format_name(reader.value.get()) : "Archive";
    reader.unchanged();
    return result;
  }
  Fd directoryAt(int root, const std::string& path) {
    auto current = opened(::fcntl(root, F_DUPFD_CLOEXEC, 0));
    for (const auto& name : parts(path)) {
      if (::mkdirat(current.get(), name.c_str(), 0700) != 0 && errno != EEXIST) {
        systemError("Could not create extracted folder");
      }
      current = opened(::openat(current.get(), name.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    }
    return current;
  }
  void forEachChild(int fd, Work& work, const std::function<void(const std::string&)>& visit) {
    const int duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
      systemError("Could not inspect archive source");
    }
    struct CloseDirectory {
      void operator()(DIR* value) const { ::closedir(value); }
    };
    const std::unique_ptr<DIR, CloseDirectory> stream(::fdopendir(duplicate));
    if (!stream) {
      ::close(duplicate);
      systemError("Could not inspect archive source");
    }
    errno = 0;
    while (const auto* entry = ::readdir(stream.get())) {
      work.check();
      if (std::string_view(entry->d_name) != "." && std::string_view(entry->d_name) != "..") {
        visit(entry->d_name);
      }
      errno = 0;
    }
    if (errno) {
      systemError("Could not enumerate archive source");
    }
  }
  void extractData(Reader& reader, int output, std::uint64_t& total, std::optional<std::uint64_t> expected) {
    std::uint64_t end = 0;
    for (;;) {
      reader.work.check();
      const void* data = nullptr;
      std::size_t size = 0;
      la_int64_t offset = 0;
      const auto status = archive_read_data_block(reader.value.get(), &data, &size, &offset);
      if (status == ARCHIVE_EOF) {
        break;
      }
      requireArchive(status, reader.value.get(), reader.work);
      if (offset < 0
          || static_cast<std::uint64_t>(offset) < end
          || static_cast<std::uint64_t>(offset) > reader.work.limits.maxExpandedBytes
          || size > reader.work.limits.maxExpandedBytes - static_cast<std::uint64_t>(offset)) {
        throw std::runtime_error("Archive data exceeds its output limit.");
      }
      const auto next = static_cast<std::uint64_t>(offset) + size;
      if (next - end > reader.work.limits.maxExpandedBytes - total) {
        throw std::runtime_error("Archive exceeds its expanded-size limit.");
      }
      total += next - end;
      end = next;
      std::size_t done = 0;
      while (done < size) {
        const auto count = ::pwrite(
            output, static_cast<const char*>(data) + done, size - done, offset + static_cast<la_int64_t>(done)
        );
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          systemError("Could not write extracted data");
        }
        done += static_cast<std::size_t>(count);
      }
      reader.work.progress.bytes.store(total);
    }
    const auto length = expected.value_or(end);
    if (length < end
        || length > reader.work.limits.maxExpandedBytes
        || length - end > reader.work.limits.maxExpandedBytes - total) {
      throw std::runtime_error("Archive file size is inconsistent or exceeds the output limit.");
    }
    total += length - end;
    if (::ftruncate(output, static_cast<off_t>(length)) != 0 || ::fsync(output) != 0) {
      systemError("Could not finish extracted data");
    }
  }
  struct Writer {
    Work& work;
    Fd output;
    std::uint64_t bytes = 0;
    std::size_t metadataBytes = 0;
    std::map<std::string, std::string> links;
    std::unique_ptr<struct archive, decltype(&archive_write_free)> value{archive_write_new(), &archive_write_free};
    Writer(const std::string& filename, ArchiveFormat format, Work& request)
        : work(request),
          output(opened(::open(filename.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600))) {
      if (format == ArchiveFormat::ZIP) {
        requireArchive(archive_write_set_format_zip(value.get()), value.get(), work);
      } else {
        requireArchive(archive_write_set_format_pax_restricted(value.get()), value.get(), work);
      }
      requireArchive(
          format == ArchiveFormat::TarGzip ? archive_write_add_filter_gzip(value.get())
                                           : archive_write_add_filter_none(value.get()),
          value.get(), work
      );
      requireArchive(archive_write_set_bytes_per_block(value.get(), 0), value.get(), work);
      requireArchive(
          archive_write_open(
              value.get(), this, nullptr,
              [](struct archive* a, void* data, const void* buffer, std::size_t size) -> la_ssize_t {
                auto& writer = *static_cast<Writer*>(data);
                try {
                  writer.work.check();
                  if (size > writer.work.limits.maxArchiveBytes - writer.bytes) {
                    throw std::runtime_error("Compressed archive exceeds the staging-size limit.");
                  }
                  std::size_t done = 0;
                  while (done < size) {
                    const auto count =
                        ::write(writer.output.get(), static_cast<const char*>(buffer) + done, size - done);
                    if (count < 0 && errno == EINTR) {
                      continue;
                    }
                    if (count <= 0) {
                      systemError("Could not write compressed archive");
                    }
                    done += static_cast<std::size_t>(count);
                  }
                  writer.bytes += size;
                  return static_cast<la_ssize_t>(size);
                } catch (...) {
                  writer.work.failure = std::current_exception();
                  archive_set_error(a, EIO, "Archive write failed");
                  return -1;
                }
              },
              nullptr
          ),
          value.get(), work
      );
    }
  };
  void compressItem(
      int parent, const std::string& name, const std::string& path, Writer& writer, std::set<std::string>& names,
      unsigned depth
  ) {
    auto& work = writer.work;
    work.item();
    if (depth > work.limits.maxDepth || path.size() > 4096 || !names.insert(path).second) {
      throw std::runtime_error("Archive paths are duplicated or too deep.");
    }
    (void)safePath(path, work);
    writer.metadataBytes += path.size();
    if (writer.metadataBytes > work.limits.maxMetadataBytes) {
      throw std::runtime_error("Archive source names exceed the metadata limit.");
    }
    const auto before = statAt(parent, name);
    const std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> entry(
        archive_entry_new(), &archive_entry_free
    );
    archive_entry_set_pathname(entry.get(), path.c_str());
    archive_entry_set_perm(entry.get(), before.st_mode & 0777);
    archive_entry_set_mtime(entry.get(), before.st_mtim.tv_sec, before.st_mtim.tv_nsec);
    if (S_ISDIR(before.st_mode)) {
      archive_entry_set_filetype(entry.get(), AE_IFDIR);
      archive_entry_set_size(entry.get(), 0);
      requireArchive(archive_write_header(writer.value.get(), entry.get()), writer.value.get(), work);
      const auto folder = opened(::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      if (stamp(statFd(folder.get())) != stamp(before)) {
        throw std::runtime_error("An archive source changed.");
      }
      forEachChild(folder.get(), work, [&](const std::string& child) {
        auto childPath = path;
        childPath += '/';
        childPath += child;
        compressItem(folder.get(), child, childPath, writer, names, depth + 1);
      });
      if (stamp(statFd(folder.get())) != stamp(before)) {
        throw std::runtime_error("An archive source folder changed.");
      }
    } else if (S_ISREG(before.st_mode)) {
      if (before.st_size < 0) {
        throw std::runtime_error("An archive source has an invalid size.");
      }
      work.account(static_cast<std::uint64_t>(before.st_size));
      archive_entry_set_filetype(entry.get(), AE_IFREG);
      archive_entry_set_size(entry.get(), before.st_size);
      requireArchive(archive_write_header(writer.value.get(), entry.get()), writer.value.get(), work);
      const auto input = opened(::openat(parent, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
      if (stamp(statFd(input.get())) != stamp(before)) {
        throw std::runtime_error("An archive source changed.");
      }
      std::array<char, 65536> buffer{};
      auto remaining = static_cast<std::uint64_t>(before.st_size);
      while (remaining) {
        work.check();
        ssize_t count;
        do {
          count = ::read(input.get(), buffer.data(), std::min<std::uint64_t>(remaining, buffer.size()));
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
          throw std::runtime_error("An archive source could not be read completely.");
        }
        std::size_t done = 0;
        while (done < static_cast<std::size_t>(count)) {
          const auto written =
              archive_write_data(writer.value.get(), buffer.data() + done, static_cast<std::size_t>(count) - done);
          if (written <= 0) {
            archiveError(writer.value.get(), work);
          }
          done += static_cast<std::size_t>(written);
        }
        remaining -= static_cast<std::uint64_t>(count);
        work.progress.bytes.fetch_add(static_cast<std::uint64_t>(count));
      }
      if (stamp(statFd(input.get())) != stamp(before)) {
        throw std::runtime_error("An archive source changed while reading.");
      }
    } else if (S_ISLNK(before.st_mode)) {
      std::array<char, 4097> target{};
      const auto size = ::readlinkat(parent, name.c_str(), target.data(), target.size());
      if (size < 0 || size >= static_cast<ssize_t>(target.size())) {
        throw std::runtime_error("Could not read archive source link.");
      }
      const std::string link(target.data(), static_cast<std::size_t>(size));
      (void)linkTarget(path, link, work);
      writer.metadataBytes += link.size();
      if (writer.metadataBytes > work.limits.maxMetadataBytes) {
        throw std::runtime_error("Archive link names exceed the metadata limit.");
      }
      writer.links.emplace(path, link);
      archive_entry_set_filetype(entry.get(), AE_IFLNK);
      archive_entry_set_size(entry.get(), 0);
      archive_entry_set_symlink(entry.get(), link.c_str());
      requireArchive(archive_write_header(writer.value.get(), entry.get()), writer.value.get(), work);
    } else {
      throw std::runtime_error("Special files cannot be compressed.");
    }
    if (stamp(statAt(parent, name)) != stamp(before)) {
      throw std::runtime_error("An archive source was replaced.");
    }
  }
} // namespace

std::span<const files::ArchiveFormatInfo> files::archiveFormats() {
  static const auto formats = [] {
    std::vector<ArchiveFormatInfo> result;
    for (const auto& item : std::array{
             ArchiveFormatInfo{ArchiveFormat::ZIP, "ZIP", ".zip"}, ArchiveFormatInfo{ArchiveFormat::TAR, "TAR", ".tar"},
             ArchiveFormatInfo{ArchiveFormat::TarGzip, "TAR gzip", ".tar.gz"}
         }) {
      const std::unique_ptr<struct archive, decltype(&archive_write_free)> writer(
          archive_write_new(), &archive_write_free
      );
      const auto format = item.format == ArchiveFormat::ZIP ? archive_write_set_format_zip(writer.get())
                                                            : archive_write_set_format_pax_restricted(writer.get());
      const auto filter = item.format == ArchiveFormat::TarGzip ? archive_write_add_filter_gzip(writer.get())
                                                                : archive_write_add_filter_none(writer.get());
      if (format == ARCHIVE_OK && filter == ARCHIVE_OK) {
        result.push_back(item);
      }
    }
    return result;
  }();
  return formats;
}
files::archive_work::Prepared
files::archive_work::inspect(const Location& source, ArchiveLimits limits, GCancellable* cancel, Progress& progress) {
  const ArchiveLocale locale;
  auto work = workFor(limits, cancel, progress);
  Reader reader(source, work);
  return {.stage = {}, .catalog = ::catalog(reader), .output = std::nullopt};
}
files::archive_work::Prepared files::archive_work::compress(
    const std::vector<Location>& sources, const Location& destination, ArchiveFormat format, ArchiveLimits limits,
    GCancellable* cancel, Progress& progress
) {
  const ArchiveLocale locale;
  auto work = workFor(limits, cancel, progress);
  if (std::ranges::find(archiveFormats(), format, &ArchiveFormatInfo::format) == archiveFormats().end()) {
    throw std::runtime_error("Archive write format is unavailable.");
  }
  const auto [parent, name] = destinationParts(destination);
  (void)parent;
  Stage stage;
  const auto output = fs::path(stage.path) / name;
  Writer writer(output.native(), format, work);
  std::set<std::string> names;
  for (const auto& source : sources) {
    if (source == destination) {
      throw std::runtime_error("An archive cannot replace one of its inputs.");
    }
    const fs::path path(native(source));
    const auto inputParent = opened(::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    compressItem(inputParent.get(), path.filename().native(), path.filename().native(), writer, names, 0);
  }
  validateLinks(writer.links, work);
  requireArchive(archive_write_close(writer.value.get()), writer.value.get(), work);
  if (::fsync(writer.output.get()) != 0) {
    systemError("Could not synchronize the archive");
  }
  work.check();
  auto location = Location::fromPath(output.native());
  return {.stage = stage.release(), .catalog = {}, .output = std::move(location), .expandedBytes = work.expanded};
}
files::archive_work::Prepared files::archive_work::extract(
    const Location& source, const Location& destination, const std::vector<std::string>& selected,
    const std::shared_ptr<const ArchiveCatalog>& expected, bool open, ArchiveLimits limits, GCancellable* cancel,
    Progress& progress
) {
  const ArchiveLocale locale;
  auto work = workFor(limits, cancel, progress);
  Reader scan(source, work);
  auto contents = ::catalog(scan);
  if (expected && (expected->source != source || expected->stamp != contents->stamp)) {
    throw std::runtime_error("The archive changed. Reload its contents before extracting.");
  }
  if (open) {
    work.limits.maxExpandedBytes = std::min<std::uint64_t>(work.limits.maxExpandedBytes, 128U * 1024U * 1024U);
  }
  const auto destinationName = open ? "member" : destinationParts(destination).second;
  Stage stage;
  const auto containerPath = fs::path(stage.path) / "payload";
  if (::mkdir(containerPath.c_str(), 0700) != 0) {
    systemError("Could not prepare archive staging");
  }
  const auto payloadPath = containerPath / destinationName;
  const auto dataPath = fs::path(stage.path) / ".data";
  if (::mkdir(payloadPath.c_str(), 0700) != 0 || ::mkdir(dataPath.c_str(), 0700) != 0) {
    systemError("Could not prepare archive staging");
  }
  const auto payload = opened(::open(payloadPath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  const auto data = opened(::open(dataPath.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  std::map<std::string, std::size_t> entries;
  for (std::size_t i = 0; i < contents->entries.size(); ++i) {
    entries.emplace(contents->entries[i].path, i);
  }
  std::set<std::string> wanted;
  const std::set<std::string> chosen(selected.begin(), selected.end());
  for (const auto& path : chosen) {
    if (!entries.contains(path)) {
      throw std::runtime_error("An archive selection no longer exists.");
    }
  }
  for (const auto& entry : contents->entries) {
    work.check();
    bool include = chosen.empty() || chosen.contains(entry.path);
    for (auto parent = entry.parent; !include && !parent.empty(); parent = parentName(parent)) {
      include = chosen.contains(parent);
    }
    if (include) {
      wanted.insert(entry.path);
    }
  }
  if (open
      && (wanted.size() != 1
          || contents->entries[entries.at(*wanted.begin())].kind == ArchiveEntryKind::DIRECTORY
          || contents->entries[entries.at(*wanted.begin())].kind == ArchiveEntryKind::SYMLINK)) {
    throw std::runtime_error("Select one regular archive member to open.");
  }
  const auto body = [&](const std::string& path) {
    std::set<std::string> seen;
    auto current = path;
    for (unsigned depth = 0; depth <= work.limits.maxDepth; ++depth) {
      work.check();
      if (!seen.insert(current).second || !entries.contains(current)) {
        throw std::runtime_error("Archive has a missing or cyclic hard-link target.");
      }
      const auto& entry = contents->entries[entries.at(current)];
      if (entry.kind == ArchiveEntryKind::FILE) {
        return current;
      }
      if (entry.kind != ArchiveEntryKind::HARDLINK) {
        throw std::runtime_error("Archive hard link does not target a regular file.");
      }
      current = entry.link;
    }
    throw std::runtime_error("Archive hard-link chain is too deep.");
  };
  std::set<std::string> bodies;
  for (const auto& path : wanted) {
    const auto& entry = contents->entries[entries.at(path)];
    if (entry.kind == ArchiveEntryKind::FILE || entry.kind == ArchiveEntryKind::HARDLINK) {
      bodies.insert(body(path));
    }
  }
  Reader reader(source, work);
  if (reader.original != contents->stamp) {
    throw std::runtime_error("Archive changed before extraction.");
  }
  struct archive_entry* header = nullptr;
  std::uint64_t total = 0;
  std::map<std::string, std::uint64_t> bodySizes;
  while (reader.next(header)) {
    const auto path = safePath(bounded(archive_entry_pathname(header)), work);
    if (bodies.contains(path)) {
      const auto& entry = contents->entries[entries.at(path)];
      const auto output = opened(
          ::openat(
              data.get(), std::to_string(entries.at(path)).c_str(),
              O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600
          )
      );
      extractData(reader, output.get(), total, entry.size);
      bodySizes.emplace(path, static_cast<std::uint64_t>(statFd(output.get()).st_size));
      const auto mode = open ? 0600U : 0600U | (entry.mode & 0155U);
      if (::fchmod(output.get(), static_cast<mode_t>(mode)) != 0) {
        systemError("Could not set extracted permissions");
      }
    } else {
      reader.skip();
    }
  }
  reader.unchanged();
  std::uint64_t logicalBytes = 0;
  for (const auto& path : wanted) {
    work.check();
    const auto& entry = contents->entries[entries.at(path)];
    if (entry.kind == ArchiveEntryKind::FILE || entry.kind == ArchiveEntryKind::HARDLINK) {
      const auto bytes = bodySizes.at(body(path));
      if (bytes > work.limits.maxExpandedBytes - logicalBytes) {
        throw std::runtime_error("Archive hard links exceed the expanded-size limit.");
      }
      logicalBytes += bytes;
    }
  }
  for (const auto& path : wanted) {
    work.check();
    const auto& entry = contents->entries[entries.at(path)];
    auto parent = directoryAt(payload.get(), entry.parent);
    if (entry.kind == ArchiveEntryKind::DIRECTORY) {
      auto folder = directoryAt(payload.get(), path);
      if (::fchmod(folder.get(), static_cast<mode_t>(0700U | (entry.mode & 0055U))) != 0) {
        systemError("Could not set folder permissions");
      }
    } else if (entry.kind == ArchiveEntryKind::FILE || entry.kind == ArchiveEntryKind::HARDLINK) {
      const auto id = std::to_string(entries.at(body(path)));
      if (::linkat(data.get(), id.c_str(), parent.get(), entry.name.c_str(), 0) != 0) {
        systemError("Could not publish extracted file");
      }
    }
  }
  for (const auto& path : wanted) {
    const auto& entry = contents->entries[entries.at(path)];
    if (entry.kind == ArchiveEntryKind::SYMLINK) {
      (void)linkTarget(path, entry.link, work);
      auto parent = directoryAt(payload.get(), entry.parent);
      if (::symlinkat(entry.link.c_str(), parent.get(), entry.name.c_str()) != 0) {
        systemError("Could not create extracted link");
      }
    }
  }
  work.check();
  const auto output =
      open ? Location::fromPath((payloadPath / *wanted.begin()).native()) : Location::fromPath(payloadPath.native());
  return {.stage = stage.release(), .catalog = std::move(contents), .output = output, .expandedBytes = logicalBytes};
}
std::string files::archive_work::cleanup(const std::vector<std::string>& paths) {
  std::string error;
  for (const auto& path : paths) {
    std::error_code result;
    fs::remove_all(path, result);
    if (result) {
      error = "Could not remove private archive staging: " + result.message();
    }
  }
  return error;
}
