# Packaging Noctalia Files

Notes for distribution packagers.
The dependency and install declarations in [meson.build](meson.build), [lib/meson.build](lib/meson.build) and [nix/package.nix](nix/package.nix) are the source of truth. This guide describes the current implementation; planned features are not packaging requirements.

## Package description

Use the description from the Nix package metadata:

> A native Wayland file manager using Noctalia rendering and controls (in development)

## Identity

| Field                  | Value                                                                 |
| ---------------------- | --------------------------------------------------------------------- |
| Package / binary       | `noctalia-files`                                                      |
| Application name       | Noctalia Files                                                        |
| Wayland application ID | `dev.noctalia.Files`                                                  |
| Homepage               | <https://github.com/noctalia-dev/noctalia-files>                      |
| Version                | [VERSION](VERSION), read by Meson                                     |
| License                | MIT ([LICENSE](LICENSE)); preserve the bundled Tabler font notice too |
| D-Bus interface / name | `org.freedesktop.FileManager1`                                        |

Files is a Linux Wayland application. It uses OpenGL ES and Pango/Cairo directly, without a Qt or GTK widget layer. GdkPixbuf and Glycin are image-decoding dependencies.

The Nix flake declares `x86_64-linux` and `aarch64-linux`. Builds and runtime checks have been performed on x86_64; ARM outputs have only been evaluated. Other architectures and musl are not currently verified. Do not copy Noctalia's architecture or libc support claims into Files package metadata.

## Build

Required tools are Meson 1.1 or newer, Ninja, pkg-config, wayland-scanner, a C compiler and a C++23 compiler with a compatible standard library. Files uses library facilities including `std::print`, `std::expected` and `std::move_only_function`; accepting `-std=c++23` alone is insufficient. GCC/libstdc++ 16.2 is verified. An older compiler minimum and Clang/libc++ compatibility have not been established for Files. Meson adds the experimental-library arguments when it detects libc++.

Use the intended final prefix at configuration time and a separate staging directory for installation:

```sh
meson setup build-package --buildtype=release --prefix=/usr \
  -Dtests=disabled -Dnative_optimizations=false
meson compile -C build-package
DESTDIR="$PWD/pkgroot" meson install --no-rebuild -C build-package
```

`just` is a development convenience, not a package build dependency. The build works from a complete source archive without repository metadata. Include `lib/`, its assets/protocols and the application `assets/` tree in that archive. Do not fetch or sync Noctalia during configuration or compilation.

Files builds its copied Noctalia UI library as static internal archives. It does not use an installed Noctalia shared library, a sibling checkout, a submodule or a running Noctalia shell. Files supplies its own GdkPixbuf/Glycin decoder; the library's optional Wuffs codec pack is not enabled. Its JPEG XL/WebP codec-pack dependencies and third-party source tree are not Files build requirements.

### Meson options

| Option                 | Default   | Packaging guidance                                                                                                                    |
| ---------------------- | --------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| `tests`                | `auto`    | Auto enables tests only for unsanitized debug builds. Use `disabled` for the ordinary release package or `enabled` for a check build. |
| `native_optimizations` | `false`   | Keep false for redistributable binaries; true adds `-march=native -mtune=native`.                                                     |
| `build_revision`       | `unknown` | Optional explicit revision supplied by the packager. No repository command runs to obtain it.                                         |

Standard Meson options such as `prefix`, `bindir`, `datadir`, `buildtype`, `b_lto` and `b_sanitize` remain available. For example, pass `-Dbuild_revision=distribution-release` when the distribution wants explicit build metadata.

Release builds enable section garbage collection and `--as-needed`. LTO is optional. GNU LTO requires compiler-aware archive tools such as `gcc-ar` and `gcc-ranlib`; the Nix package configures these explicitly. Configure a fresh build directory when changing the toolchain.

## Dependencies

### Build and link dependencies

Names below are pkg-config modules or header paths, not universal distribution package names. Install their development files as required by the distribution.

| Dependency                                        | Requirement / purpose                                                                          |
| ------------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| `wayland-protocols`                               | 1.49 or newer; build-time protocol definitions                                                 |
| `wayland-client`, `wayland-egl`, `wayland-cursor` | Wayland connection, EGL surfaces and cursor fallback                                           |
| `egl`, `glesv2`                                   | EGL and OpenGL ES; the shared library also has fallback detection through wayland-egl/libepoxy |
| `gio-2.0`                                         | 2.76 or newer; GLib/GIO filesystem and desktop integration                                     |
| `glib-2.0`                                        | GLib core services                                                                             |
| `freetype2`, `fontconfig`                         | Font discovery and rendering                                                                   |
| `cairo`, `cairo-ft`                               | Cairo rendering with FreeType integration                                                      |
| `pango`, `pangocairo`, `pangoft2`                 | Pango 1.50 or newer, including Cairo/FreeType integration                                      |
| `harfbuzz`, `xkbcommon`                           | Text shaping and keyboard input                                                                |
| `gdk-pixbuf-2.0`                                  | 2.44 or newer, built with Glycin support                                                       |
| `glycin-2`                                        | 2.0.1 or newer, compatible with the chosen GdkPixbuf build                                     |
| `librsvg-2.0`, `libpng`                           | SVG/theme artwork and PNG cache support                                                        |
| `libarchive`, `liblzma`                           | Native archive handling and bounded XZ decoding                                                |
| `libacl`                                          | POSIX permission/ACL support                                                                   |
| `tomlplusplus`                                    | 3.4 or newer                                                                                   |
| `nlohmann_json`                                   | JSON support                                                                                   |
| `stb/stb_image_resize2.h`                         | System stb resize2 header; the older resize header is insufficient                             |
| Threads                                           | System thread support                                                                          |

Use mutually compatible GdkPixbuf, Glycin and loader packages. Meson's minimum checks do not establish compatibility between every later release. The pinned Nix package selects GdkPixbuf 2.44.6 with Glycin 2.1 and builds librsvg against that same GdkPixbuf variant; that is a Nix compatibility choice, not a request for every distribution to downgrade. Installed Glycin loaders determine raster format coverage. AVIF/HEIF can require an additional loader dependency such as libheif.

### Runtime integration and helpers

The linked libraries need their ordinary runtime packages. The following data, providers and executables supply additional features:

| Runtime component                             | Role                                                                              |
| --------------------------------------------- | --------------------------------------------------------------------------------- |
| Wayland compositor with EGL/OpenGL ES support | Graphical window; Noctalia and Umbriel are not required                           |
| Fontconfig and usable text fonts              | Application text; the bundled glyph font does not replace ordinary fonts          |
| Shared MIME database                          | Type descriptions, application associations and provider format detection         |
| Icon theme                                    | File, folder and application artwork; the Nix wrapper supplies Adwaita            |
| Glycin loaders                                | Raster images in thumbnails, previews and metadata                                |
| Session D-Bus and GVfs                        | Trash and supported mounted remote locations                                      |
| `glib-networking`                             | GIO TLS implementation for secure WebDAV                                          |
| `ffmpeg`, `ffprobe`                           | Video/audio artwork and media metadata                                            |
| `pdftoppm`, `pdfinfo`                         | PDF thumbnails and metadata, normally supplied by Poppler utilities               |
| `7zz`, `7z` or `7za`                          | System 7-Zip CLI for encrypted 7z input, searched in that order                   |
| `xdg-terminal-exec`                           | Open Terminal Here; unavailable when the helper or a suitable terminal is missing |
| Associated applications                       | Opening files through the user's GIO MIME associations                            |

Missing optional helpers make their associated feature unavailable; they do not prevent ordinary native directory browsing. Recommend or depend on them according to the features the package advertises. The Nix wrapper exposes FFmpeg, Poppler utilities, 7-Zip and xdg-terminal-exec on `PATH`.

Archive support uses the system libarchive and its enabled formats/filters. Unencrypted RAR input depends on that build's capabilities; encrypted RAR is outside the implemented scope. Encrypted 7z input uses the system CLI through the bounded worker transport. There is no py7zr requirement, 7-Zip SDK build dependency, `7z.so` path or private C++ ABI contract. `NOCTALIA_FILES_7Z` can select an absolute executable path for an unusual installation. File Roller, Ark, unrar and a Samba server are not required by Files.

### GVfs providers across distributions

Files uses GIO directly, so these transfers do not require exposing remote mounts through FUSE. `gvfs-fuse` is useful when other applications need POSIX paths into those mounts. A session D-Bus connection and the corresponding backend must be available. The current Files UI uses locations already mounted/authenticated through another GIO client; its own connect-to-server and credential dialogs are pending.

The following provider packages were checked in official catalogs on 2026-11-10. These are runtime feature dependencies, not additional C++ headers. Their package managers supply the supporting client libraries.

| Distribution / catalog | SMB and WebDAV providers                                                                                                                                                                       | TLS                                                                                              |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| Arch Extra             | `gvfs`, [`gvfs-smb`](https://archlinux.org/packages/extra/x86_64/gvfs-smb/) and [`gvfs-dnssd`](https://archlinux.org/packages/extra/x86_64/gvfs-dnssd/files/); the latter contains `gvfsd-dav` | [`glib-networking`](https://archlinux.org/packages/extra/x86_64/glib-networking/)                |
| Fedora 44/45           | [`gvfs`](https://packages.fedoraproject.org/pkgs/gvfs/gvfs/fedora-44.html) includes WebDAV; [`gvfs-smb`](https://packages.fedoraproject.org/pkgs/gvfs/gvfs-smb/) adds SMB                      | [`glib-networking`](https://packages.fedoraproject.org/pkgs/glib-networking/glib-networking/)    |
| Debian 13              | `gvfs` and [`gvfs-backends`](https://packages.debian.org/trixie/gvfs-backends)                                                                                                                 | [`glib-networking`](https://packages.debian.org/trixie/glib-networking)                          |
| Ubuntu 26.04 LTS       | `gvfs` and [`gvfs-backends`](https://packages.ubuntu.com/resolute/gvfs-backends)                                                                                                               | [`glib-networking`](https://packages.ubuntu.com/resolute/glib-networking)                        |
| Alpine 3.22 community  | `gvfs`, [`gvfs-smb`](https://pkgs.alpinelinux.org/package/v3.22/community/x86_64/gvfs-smb) and [`gvfs-dav`](https://pkgs.alpinelinux.org/package/v3.22/community/x86_64/gvfs-dav)              | [`glib-networking`](https://pkgs.alpinelinux.org/package/v3.22/community/x86_64/glib-networking) |
| Pinned Nixpkgs         | `gvfs` supplies both backends; private real-server checks pass                                                                                                                                 | `glib-networking` in the Files wrapper/dev shell                                                 |

Catalog availability does not establish that a distribution release satisfies all the other build requirements or that Files has been tested on that distro. openSUSE catalog/spec access was blocked during the audit, so its full package split is not marked verified here.

Normal native packages use the distribution's GIO module search paths. Do not copy Nix store paths or require users to export `GIO_EXTRA_MODULES`. The Nix wrapper and dev shell set it because those modules live in separate store outputs. The assignment inside `flake.nix`'s `preCheck` is scoped to that test derivation. TLS validation remains with the provider and system trust configuration.

## Installed layout

With the default relative `datadir=share`, Meson installs:

```text
<prefix>/bin/noctalia-files
<prefix>/share/noctalia-files/assets/fonts/noctalia-tabler.ttf
<prefix>/share/noctalia-files/assets/fonts/tabler.json
<prefix>/share/noctalia-files/assets/fonts/tabler-icons-license.txt
<prefix>/share/noctalia-files/templates/colors.toml
<prefix>/share/dbus-1/services/org.freedesktop.FileManager1.service
<prefix>/share/bash-completion/completions/noctalia-files
<prefix>/share/zsh/site-functions/_noctalia-files
<prefix>/share/fish/vendor_completions.d/noctalia-files.fish
<prefix>/share/licenses/noctalia-files/LICENSE
```

The fonts directory is required at runtime. Keep the Tabler license alongside its font/metadata and preserve applicable source notices. The palette template is a Noctalia integration resource, not an automatically installed user configuration.

Current Meson rules do not install a desktop entry, application icon, AppStream/metainfo file, manual page or systemd unit. Desktop/MIME/default-file-manager registration remains planned. Do not assume the sibling projects' installed files exist here.

### Asset lookup and staging

The configured `<prefix>/<datadir>/noctalia-files/assets` path is compiled into the binary. `NOCTALIA_FILES_ASSETS_DIR` explicitly overrides it; an invalid override is an error. Files does not use the shell's portable-bundle asset discovery chain. A source build can run with:

```sh
NOCTALIA_FILES_ASSETS_DIR="$PWD/lib/assets" ./build-package/noctalia-files
```

Do not leave that development override in an installed launcher. `DESTDIR` stages files without changing the compiled runtime paths. A packaging smoke test that starts the staged binary before publication must supply its staged assets explicitly; the final installed binary should work without the override.

### D-Bus activation

The generated `org.freedesktop.FileManager1.service` invokes the configured absolute binary path with `--file-manager-service`. Keep that path aligned with the installation prefix. Its graphical activation needs the session's Wayland environment.

This is an on-demand session service, not an always-running background daemon. Installing it does not register an `inode/directory` MIME default. The generic service name can overlap another file manager's activation provider; handle provider selection through the distribution's packaging policy rather than silently overwriting another package's file.

### Shell completions

Meson installs the static Bash, Zsh and Fish scripts listed above. The binary also supports `completions bash`, `completions zsh` and `completions fish` without a compositor. Packaging does not need to execute the target binary to generate completion files, which also permits cross builds to install them.

## Configuration and writable data

The application loads internal defaults, then the user's base configuration and GUI overrides:

| Path                                            | Ownership                                            |
| ----------------------------------------------- | ---------------------------------------------------- |
| `$XDG_CONFIG_HOME/noctalia-files/config.toml`   | User/declarative base configuration                  |
| `$XDG_CONFIG_HOME/noctalia-files/settings.toml` | Writable GUI preference overrides                    |
| `$XDG_CONFIG_HOME/noctalia-files/colors.toml`   | Optional palette handoff                             |
| `$XDG_STATE_HOME/noctalia-files/state.toml`     | Application-owned persistent state, including Places |
| `$XDG_CACHE_HOME/thumbnails/`                   | Regenerable freedesktop thumbnail cache              |

Unset XDG config/state/cache roots use `~/.config`, `~/.local/state` and `~/.cache`. Files does not currently search `/etc/xdg` or a packaged default config. Do not install per-user `settings.toml`, state or caches from a system package. Keep the GUI override file writable even when `config.toml` is an immutable declarative file. Valid external configuration changes apply automatically.

Validate a candidate base configuration without a compositor:

```sh
noctalia-files config validate /path/to/config.toml
```

## Nix integration

[nix/package.nix](nix/package.nix) is the package recipe; [flake.nix](flake.nix) supplies the package, dev shell, checks, overlay and NixOS/Home Manager/hjem module exports. [default.nix](default.nix) exposes the non-flake package/module entry points.

```sh
nix build path:.#default
nix develop path:. -c just build debug
nix flake check path:.
```

The wrapper supplies runtime helper paths, GVfs/TLS modules, Glycin loader data, the shared MIME database and Adwaita icons without requiring the Noctalia shell. Assets use the package's own compiled store path. Home Manager and hjem can generate `config.toml` from an attribute set, TOML string or file path and validate it at build time when a package is selected. Their empty default settings leave the base file absent. GUI overrides stay separate. The NixOS module installs the selected package; none of these modules installs an always-running Files service or changes the default file manager.

## Verification

Enable the headless suite explicitly for a release check build:

```sh
meson setup build-check --buildtype=release --prefix=/usr -Dtests=enabled
meson compile -C build-check
meson test -C build-check --print-errorlogs
```

Tests additionally require Python 3, dbus-daemon, `wayland-server` development files and libwebp development files. Provide the media/PDF/archive helpers and Glycin loaders exercised by the fixtures, including AVIF/HEIF support. A compositor is not needed for the ordinary headless suites. Their local-VFS setting does not verify GVfs network providers; live provider checks use separate private fixtures.

Before publishing, check `--help`, `--version`, configuration validation, installed fonts and completions. Test an installed window with the development asset override unset, plus the image/media/archive and GVfs features advertised by the package. Keep packaging smoke tests isolated from users' configuration, desktop input and real files. x86_64 Nix release builds and isolated Wayland/runtime checks are verified; ARM and a full distro installation matrix remain follow-ups.

## Contact

- Issues: <https://github.com/noctalia-dev/noctalia-files/issues>
- Discord: <https://discord.noctalia.dev>
