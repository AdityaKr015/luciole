// Uses system 7-Zip SDK interfaces only. No decoder implementation is bundled.
// GUID definitions must precede the SDK interface headers.
// clang-format off
#include "CPP/Common/MyInitGuid.h"
// clang-format on
#include "CPP/7zip/Archive/IArchive.h"
#include "CPP/7zip/ICoder.h"
#include "CPP/7zip/IPassword.h"
#include "CPP/7zip/PropID.h"
#include "archive_sevenzip_abi.h"
#include "noctalia_files_archive_worker_config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <dlfcn.h>
#include <glib.h>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  using Json = nlohmann::json;
  using AllocString = BSTR (*)(const OLECHAR*);
  using ClearVariant = HRESULT (*)(PROPVARIANT*);
  using StringLength = UINT (*)(BSTR);
  AllocString allocateString;
  ClearVariant clearVariant;
  StringLength stringLength;
  constexpr int unavailable = 69, unsupported = 70, passwordFailure = 2;

  void require(bool condition) {
    if (!condition) {
      throw std::runtime_error("Invalid archive provider input or output");
    }
  }
  std::uint64_t number(std::string_view value) {
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    require(error == std::errc{} && end == value.data() + value.size());
    return result;
  }
  bool same(const GUID& left, const GUID& right) { return std::memcmp(&left, &right, sizeof(GUID)) == 0; }
  template <typename Interface> struct Reference {
    Interface* value = nullptr;
    ~Reference() {
      if (value) {
        value->Release();
      }
    }
  };
  // Each concrete callback has its own refcount, including multiple-interface objects.
#define FILES_SEVENZIP_REFERENCES                                                                                      \
  ULONG AddRef() noexcept override { return ++references; }                                                            \
  ULONG Release() noexcept override {                                                                                  \
    const auto remaining = --references;                                                                               \
    if (!remaining) {                                                                                                  \
      delete this;                                                                                                     \
    }                                                                                                                  \
    return remaining;                                                                                                  \
  }                                                                                                                    \
  std::atomic<ULONG> references{1}

  struct Source final : IInStream {
    UInt64 position = 0, bytes = 0, size = 0, budget = 0;
    explicit Source(std::uint64_t maximum) {
      struct stat info{};
      require(::fstat(3, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0);
      size = static_cast<UInt64>(info.st_size);
      require(size <= maximum && maximum <= std::numeric_limits<UInt64>::max() / 4);
      budget = maximum * 4;
    }
    HRESULT QueryInterface(const GUID& id, void** output) noexcept override {
      *output = nullptr;
      if (!same(id, IID_IUnknown) && !same(id, IID_IInStream) && !same(id, IID_ISequentialInStream)) {
        return E_NOINTERFACE;
      }
      *output = static_cast<IInStream*>(this);
      AddRef();
      return S_OK;
    }
    FILES_SEVENZIP_REFERENCES;
    HRESULT Read(void* output, UInt32 requested, UInt32* processed) noexcept override {
      if (processed) {
        *processed = 0;
      }
      const auto count = std::min<UInt64>({requested, 65536, size - position});
      if (count > budget - bytes) {
        return E_FAIL;
      }
      ssize_t received;
      do {
        received = ::pread(3, output, static_cast<std::size_t>(count), static_cast<off_t>(position));
      } while (received < 0 && errno == EINTR);
      if (received < 0 || (!received && count)) {
        return E_FAIL;
      }
      position += static_cast<UInt64>(received);
      bytes += static_cast<UInt64>(received);
      if (processed) {
        *processed = static_cast<UInt32>(received);
      }
      return S_OK;
    }
    HRESULT Seek(Int64 offset, UInt32 origin, UInt64* result) noexcept override {
      if (origin > 2) {
        return E_FAIL;
      }
      const UInt64 base = origin == 0 ? 0 : origin == 1 ? position : size;
      UInt64 next;
      if (offset < 0) {
        const auto distance = static_cast<UInt64>(-(offset + 1)) + 1;
        if (distance > base) {
          return E_FAIL;
        }
        next = base - distance;
      } else {
        if (static_cast<UInt64>(offset) > size - base) {
          return E_FAIL;
        }
        next = base + static_cast<UInt64>(offset);
      }
      position = next;
      if (result) {
        *result = position;
      }
      return S_OK;
    }
  };
  struct Secret {
    std::wstring units;
    ~Secret() { explicit_bzero(units.data(), units.size() * sizeof(wchar_t)); }
    Secret() {
      std::array<char, 1026> bytes{};
      std::size_t size = 0;
      while (size < bytes.size()) {
        const auto count = ::read(STDIN_FILENO, bytes.data() + size, bytes.size() - size);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        require(count >= 0);
        if (!count) {
          break;
        }
        size += static_cast<std::size_t>(count);
      }
      require(size > 0 && size <= 1025 && bytes[size - 1] == '\n');
      --size;
      require(
          std::string_view(bytes.data(), size).find_first_of(std::string_view("\0\r\n", 3)) == std::string_view::npos
      );
      glong length = 0;
      auto* utf16 = g_utf8_to_utf16(bytes.data(), static_cast<glong>(size), nullptr, &length, nullptr);
      explicit_bzero(bytes.data(), bytes.size());
      require(utf16 != nullptr);
      // Linux BSTR contains UTF-16 units even though wchar_t is 32 bits.
      units.reserve(static_cast<std::size_t>(length));
      for (glong i = 0; i < length; ++i) {
        units.push_back(static_cast<wchar_t>(utf16[i]));
      }
      explicit_bzero(utf16, static_cast<std::size_t>(length) * sizeof(gunichar2));
      g_free(utf16);
    }
    HRESULT get(BSTR* output) const noexcept {
      *output = allocateString(units.c_str());
      return *output ? S_OK : E_OUTOFMEMORY;
    }
  };
  struct Open final : IArchiveOpenCallback, ICryptoGetTextPassword {
    const Secret& secret;
    bool requested = false;
    explicit Open(const Secret& value) : secret(value) {}
    HRESULT QueryInterface(const GUID& id, void** output) noexcept override {
      *output = nullptr;
      if (same(id, IID_IUnknown) || same(id, IID_IArchiveOpenCallback)) {
        *output = static_cast<IArchiveOpenCallback*>(this);
      } else if (same(id, IID_ICryptoGetTextPassword)) {
        *output = static_cast<ICryptoGetTextPassword*>(this);
      } else {
        return E_NOINTERFACE;
      }
      AddRef();
      return S_OK;
    }
    FILES_SEVENZIP_REFERENCES;
    HRESULT SetTotal(const UInt64*, const UInt64*) noexcept override { return S_OK; }
    HRESULT SetCompleted(const UInt64*, const UInt64*) noexcept override { return S_OK; }
    HRESULT CryptoGetTextPassword(BSTR* output) noexcept override {
      requested = true;
      return secret.get(output);
    }
  };
  struct Output final : ISequentialOutStream {
    std::uint64_t bytes = 0, maximum;
    explicit Output(std::uint64_t limit) : maximum(limit) {}
    HRESULT QueryInterface(const GUID& id, void** output) noexcept override {
      *output = nullptr;
      if (!same(id, IID_IUnknown) && !same(id, IID_ISequentialOutStream)) {
        return E_NOINTERFACE;
      }
      *output = static_cast<ISequentialOutStream*>(this);
      AddRef();
      return S_OK;
    }
    FILES_SEVENZIP_REFERENCES;
    HRESULT Write(const void* input, UInt32 size, UInt32* processed) noexcept override {
      if (processed) {
        *processed = 0;
      }
      if (size > maximum - bytes) {
        return E_FAIL;
      }
      std::size_t done = 0;
      while (done < size) {
        const auto count = ::write(STDOUT_FILENO, static_cast<const char*>(input) + done, size - done);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          return E_FAIL;
        }
        done += static_cast<std::size_t>(count);
      }
      bytes += size;
      if (processed) {
        *processed = size;
      }
      return S_OK;
    }
  };
  struct Extract final : IArchiveExtractCallback, ICryptoGetTextPassword {
    const Secret& secret;
    Output& output;
    UInt32 selected;
    Int32 operation = -1;
    explicit Extract(const Secret& password, Output& stream, UInt32 index)
        : secret(password), output(stream), selected(index) {}
    HRESULT QueryInterface(const GUID& id, void** result) noexcept override {
      *result = nullptr;
      if (same(id, IID_IUnknown) || same(id, IID_IArchiveExtractCallback) || same(id, IID_IProgress)) {
        *result = static_cast<IArchiveExtractCallback*>(this);
      } else if (same(id, IID_ICryptoGetTextPassword)) {
        *result = static_cast<ICryptoGetTextPassword*>(this);
      } else {
        return E_NOINTERFACE;
      }
      AddRef();
      return S_OK;
    }
    FILES_SEVENZIP_REFERENCES;
    HRESULT SetTotal(UInt64) noexcept override { return S_OK; }
    HRESULT SetCompleted(const UInt64*) noexcept override { return S_OK; }
    HRESULT GetStream(UInt32 index, ISequentialOutStream** result, Int32 mode) noexcept override {
      *result = nullptr;
      if (mode == 0 && index == selected) {
        output.AddRef();
        *result = &output;
      }
      return index == selected || mode != 0 ? S_OK : E_FAIL;
    }
    HRESULT PrepareOperation(Int32) noexcept override { return S_OK; }
    HRESULT SetOperationResult(Int32 result) noexcept override {
      operation = result;
      return S_OK;
    }
    HRESULT CryptoGetTextPassword(BSTR* result) noexcept override { return secret.get(result); }
  };
#undef FILES_SEVENZIP_REFERENCES

  struct Property {
    PROPVARIANT value{};
    ~Property() { clearVariant(&value); }
    explicit Property(IInArchive& archive, UInt32 index, PROPID id) {
      require(archive.GetProperty(index, id, &value) == S_OK);
    }
    std::string text(std::size_t maximum) const {
      if (value.vt == VT_EMPTY) {
        return {};
      }
      require(value.vt == VT_BSTR);
      const auto length = stringLength(value.bstrVal);
      require(length <= maximum);
      if (length == 0) {
        return {};
      }
      std::vector<gunichar2> units;
      units.reserve(length);
      for (UINT i = 0; i < length; ++i) {
        require(value.bstrVal[i] > 0 && value.bstrVal[i] <= 65535);
        units.push_back(static_cast<gunichar2>(value.bstrVal[i]));
      }
      gchar* text = g_utf16_to_utf8(units.data(), length, nullptr, nullptr, nullptr);
      require(text != nullptr);
      std::string result(text);
      g_free(text);
      require(result.size() <= maximum);
      return result;
    }
    bool boolean() const {
      require(value.vt == VT_EMPTY || value.vt == VT_BOOL);
      return value.vt == VT_BOOL && value.boolVal != VARIANT_FALSE;
    }
  };
  Json catalog(IInArchive& archive, UInt32 count, std::size_t limit) {
    Json entries = Json::array();
    std::size_t bytes = 32;
    for (UInt32 i = 0; i < count; ++i) {
      const auto path = Property(archive, i, kpidPath).text(limit);
      const bool directory = Property(archive, i, kpidIsDir).boolean();
      const bool encrypted = Property(archive, i, kpidEncrypted).boolean();
      const auto symlink = Property(archive, i, kpidSymLink).text(limit);
      const auto hardlink = Property(archive, i, kpidHardLink).text(limit);
      const Property size(archive, i, kpidSize), attributes(archive, i, kpidAttrib), posix(archive, i, kpidPosixAttrib);
      require(size.value.vt == VT_UI8 || (directory && size.value.vt == VT_EMPTY));
      require(attributes.value.vt == VT_EMPTY || attributes.value.vt == VT_UI4);
      require(posix.value.vt == VT_EMPTY || posix.value.vt == VT_UI4);
      const auto mode = posix.value.vt == VT_UI4                                ? posix.value.ulVal
          : attributes.value.vt == VT_UI4 && (attributes.value.ulVal & 0x8000U) ? attributes.value.ulVal >> 16U
          : directory                                                           ? 0755U
                                                                                : 0644U;
      // Links and special Unix types behind encryption remain deliberately unsupported.
      require(
          symlink.empty()
          && hardlink.empty()
          && ((mode & S_IFMT) == 0 || (mode & S_IFMT) == S_IFREG || (mode & S_IFMT) == S_IFDIR)
      );
      Json entry{
          {"index", i},
          {"path", path},
          {"directory", directory},
          {"encrypted", encrypted},
          {"size", size.value.vt == VT_UI8 ? size.value.uhVal.QuadPart : 0},
          {"mode", mode & 0777U}
      };
      bytes += entry.dump().size() + 1;
      require(bytes <= limit);
      entries.push_back(std::move(entry));
    }
    return Json{{"version", 1}, {"entries", std::move(entries)}};
  }
  int run(int argc, char** argv) {
    require(argc == 7);
    const bool metadata = std::string_view(argv[1]) == "catalog";
    require(metadata || std::string_view(argv[1]) == "extract");
    const auto index = number(argv[2]), limit = number(argv[3]), maximumEntries = number(argv[4]),
               inputLimit = number(argv[5]), outputLimit = number(argv[6]);
    require(limit <= 16U * 1024U * 1024U && maximumEntries <= 50000 && index <= UINT32_MAX);
    void* library = ::dlopen(NOCTALIA_FILES_SEVENZIP_LIBRARY, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      return unavailable;
    }
    // C exports are the only calls permitted before verifying the C++ interface layout.
    const auto module = reinterpret_cast<Func_GetModuleProp>(::dlsym(library, "GetModuleProp"));
    clearVariant = reinterpret_cast<ClearVariant>(::dlsym(library, "VariantClear"));
    allocateString = reinterpret_cast<AllocString>(::dlsym(library, "SysAllocString"));
    stringLength = reinterpret_cast<StringLength>(::dlsym(library, "SysStringLen"));
    const auto create = reinterpret_cast<Func_CreateObject>(::dlsym(library, "CreateObject"));
    if (!module || !clearVariant || !allocateString || !stringLength || !create) {
      return unavailable;
    }
    if (!files::sevenzip::compatibleModule(module, clearVariant)) {
      return unavailable;
    }
    const Secret secret;
    Reference<Source> source{new Source(inputLimit)};
    Reference<Open> open{new Open(secret)};
    Reference<IInArchive> archive;
    const GUID format{0x23170f69, 0x40c1, 0x278a, {0x10, 0, 0, 1, 0x10, 7, 0, 0}};
    if (create(&format, &IID_IInArchive, reinterpret_cast<void**>(&archive.value)) != S_OK || !archive.value) {
      return unavailable;
    }
    const auto opened = archive.value->Open(source.value, nullptr, open.value);
    if (opened != S_OK) {
      return open.value->requested ? passwordFailure : unsupported;
    }
    UInt32 count = 0;
    require(archive.value->GetNumberOfItems(&count) == S_OK && count <= maximumEntries);
    if (metadata) {
      const auto text = catalog(*archive.value, count, static_cast<std::size_t>(limit)).dump();
      require(text.size() <= limit);
      Reference<Output> output{new Output(limit)};
      require(output.value->Write(text.data(), static_cast<UInt32>(text.size()), nullptr) == S_OK);
      return 0;
    }
    require(index < count);
    Reference<Output> output{new Output(outputLimit)};
    Reference<Extract> extraction{new Extract(secret, *output.value, static_cast<UInt32>(index))};
    const auto selected = static_cast<UInt32>(index);
    const auto status = archive.value->Extract(&selected, 1, 0, extraction.value);
    if (extraction.value->operation == NArchive::NExtract::NOperationResult::kWrongPassword
        || extraction.value->operation == NArchive::NExtract::NOperationResult::kCRCError
        || extraction.value->operation == NArchive::NExtract::NOperationResult::kDataError) {
      return passwordFailure;
    }
    return status == S_OK
            && extraction.value->operation == NArchive::NExtract::NOperationResult::kOK
            && output.value->bytes == outputLimit
        ? 0
        : unsupported;
  }
} // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (...) {
    return unsupported;
  }
}
