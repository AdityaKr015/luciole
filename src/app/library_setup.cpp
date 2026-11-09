#include "app/library_setup.h"

#include "core/application_identity.h"
#include "core/files/resource_paths.h"
#include "core/ui_strings.h"
#include "imaging/pixbuf_image_decoder.h"
#include "noctalia_files_build_info.h"
#include "ui/palette.h"

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
    setPalette(
        {.primary = hex("#fff59b"),
         .onPrimary = hex("#0e0e43"),
         .primaryContainer = hex("#383526"),
         .onPrimaryContainer = hex("#fff59b"),
         .secondary = hex("#a9aefe"),
         .onSecondary = hex("#0e0e43"),
         .secondaryContainer = hex("#29294f"),
         .onSecondaryContainer = hex("#dfe0ff"),
         .tertiary = hex("#9bfece"),
         .onTertiary = hex("#0e0e43"),
         .tertiaryContainer = hex("#143a2b"),
         .onTertiaryContainer = hex("#9bfece"),
         .error = hex("#fd4663"),
         .onError = hex("#0e0e43"),
         .errorContainer = hex("#4f1521"),
         .onErrorContainer = hex("#ffd9df"),
         .surface = hex("#070722"),
         .onSurface = hex("#f3edf7"),
         .onSurfaceVariant = hex("#7c80b4"),
         .surfaceContainerLow = hex("#0b0b27"),
         .surfaceContainer = hex("#11112d"),
         .surfaceContainerHigh = hex("#1a1a39"),
         .surfaceContainerHighest = hex("#232345"),
         .outlineVariant = hex("#21215f"),
         .shadow = hex("#070722"),
         .scrim = hex("#000000"),
         .hover = hex("#9bfece"),
         .onHover = hex("#0e0e43")}
    );
  }
} // namespace files
