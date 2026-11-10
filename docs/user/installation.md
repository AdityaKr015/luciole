# Installation and startup

Files needs a Wayland session with EGL/OpenGL ES support. Neither the Noctalia shell nor a particular compositor is required. Distribution dependencies and package-building details are maintained in [PACKAGING.md](../../PACKAGING.md).

## Start Files

```sh
noctalia-files
noctalia-files /path/to/folder
noctalia-files 'file:///path/to/folder'
```

Without an argument, Files opens Home. Supply one directory path or URI to start elsewhere. Remote locations require the corresponding installed GIO/GVfs backend and an already mounted/authenticated location. Files' own connect-to-server and credential dialogs are not implemented yet.

Use `noctalia-files --help` for command syntax and `noctalia-files --version` for the version and build revision. These commands do not open a window. Bash, Zsh and Fish completions are installed by the package; `noctalia-files completions bash` also prints the Bash completion script.

## Build from the source directory

Use the pinned Nix development environment:

```sh
nix develop path:. -c just build release
nix develop path:. -c just run release
```

The run recipe selects the source tree's assets. Do not put that development asset override in an installed launcher.

To build the Nix package instead:

```sh
nix build path:.#default
./result/bin/noctalia-files
```

The flake exports NixOS, Home Manager and hjem modules. Home Manager and hjem can supply `programs.noctalia-files.settings` as a declarative base; GUI preferences remain separate. See [configuration](configuration.md) and [Nix packaging](../../PACKAGING.md#nix-integration). Installing Files does not automatically make it the default file manager.

## Optional features

Image format coverage depends on installed Glycin loaders. Video/audio thumbnails and metadata need FFmpeg; PDF thumbnails and metadata need Poppler utilities. Encrypted 7z input needs a system 7-Zip executable. Terminal actions need `xdg-terminal-exec` and a suitable terminal. Trash and mounted network locations need the appropriate GIO/GVfs services. Missing helpers leave ordinary local browsing available; [Packaging](../../PACKAGING.md#runtime-integration-and-helpers) lists the full requirements.
