# Installation and startup

Luciole needs a Wayland session with EGL/OpenGL ES support. Neither the Noctalia shell nor a particular compositor is required. Distribution dependencies and package-building details are maintained in [PACKAGING.md](../../PACKAGING.md).

## Start Luciole

```sh
luciole
luciole /path/to/folder
luciole 'file:///path/to/folder'
```

Without an argument, Luciole opens Home. Supply one directory path or URI to start elsewhere. Remote locations require the corresponding installed GIO/GVfs backend and an already mounted/authenticated location. Luciole' own connect-to-server and credential dialogs are not implemented yet.

Every window runs in one Luciole process. Starting `luciole` while it is already running opens a new window in that process, so additional windows cost far less memory than separate instances. A relative path names a folder under the directory where the command ran. The process exits when its last window closes.

Use `luciole --help` for command syntax and `luciole --version` for the version and build revision. These commands do not open a window. Bash, Zsh and Fish completions are installed by the package; `luciole completions bash` also prints the Bash completion script.

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
./result/bin/luciole
```

The flake exports NixOS, Home Manager and hjem modules. Home Manager and hjem can supply `programs.luciole.settings` as a declarative base; GUI preferences remain separate. See [configuration](configuration.md) and [Nix packaging](../../PACKAGING.md#nix-integration). Installing Luciole does not automatically make it the default file manager.

## Optional features

Image format coverage depends on installed Glycin loaders. Video/audio thumbnails and metadata need FFmpeg; PDF thumbnails and metadata need Poppler utilities. Encrypted 7z input needs a system 7-Zip executable. Terminal actions need `xdg-terminal-exec` and a suitable terminal. Trash and mounted network locations need the appropriate GIO/GVfs services. Missing helpers leave ordinary local browsing available; [Packaging](../../PACKAGING.md#runtime-integration-and-helpers) lists the full requirements.
