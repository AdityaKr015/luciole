#include "core/files/resource_paths.h"

#include "noctalia_files_build_info.h"

#include <cstdlib>
#include <stdexcept>

namespace paths {
  const std::filesystem::path& assetsRoot() {
    static const auto root = [] {
      const char* overridePath = std::getenv("NOCTALIA_FILES_ASSETS_DIR");
      std::filesystem::path path = overridePath != nullptr ? std::filesystem::path(overridePath)
                                                           : std::filesystem::path(NOCTALIA_FILES_INSTALL_ASSETSDIR);
      if (!std::filesystem::is_regular_file(path / "fonts/noctalia-tabler.ttf")
          || !std::filesystem::is_regular_file(path / "fonts/tabler.json")) {
        throw std::runtime_error("invalid Noctalia Files asset directory: " + path.string());
      }
      return path;
    }();
    return root;
  }

  std::filesystem::path assetPath(std::string_view relativePath) {
    return assetsRoot() / std::filesystem::path(relativePath);
  }
} // namespace paths
