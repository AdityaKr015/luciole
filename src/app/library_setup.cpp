#include "app/library_setup.h"

#include "core/application_identity.h"
#include "core/files/resource_paths.h"
#include "core/ui_strings.h"
#include "imaging/pixbuf_image_decoder.h"
#include "noctalia_files_build_info.h"
#include "ui/palette_file.h"

#include <cstdlib>
#include <filesystem>
namespace files {
  void initializeLibrary() {
    setApplicationIdentity({.name = "noctalia-files", .envPrefix = "NOCTALIA_FILES"});
    const char* overridePath = std::getenv("NOCTALIA_FILES_ASSETS_DIR");
    const std::filesystem::path assets = overridePath ? overridePath : NOCTALIA_FILES_INSTALL_ASSETSDIR;
    paths::setAssetRoots({.application = assets, .library = assets});
    setUiStrings({.keyName = {}, .selectPlaceholder = [] { return std::string("Select…"); }});
    installPixbufImageDecoder();
    // PaletteWatcher applies colors.toml once the main loop owner exists.
    setPalette(defaultPalette());
  }
} // namespace files
