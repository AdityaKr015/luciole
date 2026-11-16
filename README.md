# Luciole

Luciole is a native Wayland file manager with grid, list and Miller column views, tabs, split view and file previews.

It is built in C++23 on Wayland and OpenGL ES, using [Noctalia's](https://github.com/noctalia-dev/noctalia) rendering library and controls. It runs independently of the Noctalia shell and works with your Wayland desktop.

<p align="center">
  <img src="assets/icons/dev.noctalia.luciole.svg" alt="Luciole Logo" width="192" />
</p>

<p align="center">
  <a href="https://github.com/noctalia-dev/luciole/commits">
    <img src="https://img.shields.io/github/last-commit/noctalia-dev/luciole?style=for-the-badge&labelColor=FFF59B&color=FFF59B&logo=git&logoColor=070722&label=commit" alt="Last commit" />
  </a>
  <a href="https://github.com/noctalia-dev/luciole/stargazers">
    <img src="https://img.shields.io/github/stars/noctalia-dev/luciole?style=for-the-badge&labelColor=FFF59B&color=FFF59B&logo=github&logoColor=070722" alt="GitHub stars" />
  </a>
  <a href="docs/user/index.md">
    <img src="https://img.shields.io/badge/docs-FFF59B?style=for-the-badge&logo=gitbook&logoColor=070722&labelColor=FFF59B" alt="Documentation" />
  </a>
  <a href="https://discord.noctalia.dev">
    <img src="https://img.shields.io/badge/discord-FFF59B?style=for-the-badge&labelColor=FFF59B&logo=discord&logoColor=070722" alt="Discord" />
  </a>
</p>

## Features

- **Browsing:** grid, list and optional Miller columns, with tabs, separate windows, split view, adjustable zoom and sortable detail columns.
- **Navigation and search:** clickable breadcrumbs, path completion, recursive filename search and instant filtering of the current folder.
- **Thumbnails and previews:** image and video thumbnails, previews for images, text, PDF pages and video stills, plus file and media metadata.
- **File operations:** copy, move, links, batch rename, folder merging, conflict choices, Trash and restoration, with [Undo/Redo for supported operations](docs/user/file-operations.md#undo-and-redo).
- **Archives:** browse, create and extract archives, with password prompts for [supported encrypted formats](docs/user/file-operations.md#archives).
- **Places:** a customizable sidebar with shared desktop bookmarks, mounted drives and optional folder trees.
- **Desktop integration:** mounted SFTP, SMB and WebDAV locations, Open With, terminal actions, custom scripts and an optional [file-picker portal backend](PACKAGING.md#filechooser-portal-backend) for Open and Save dialogs.
- **Configuration:** built-in settings, automatically reloaded TOML preferences, desktop icon themes and configurable colors.

Luciole is under active development. The [user guide](docs/user/index.md) describes current behavior.

## Install and run

See the [installation guide](docs/user/installation.md) for setup and startup, and [PACKAGING.md](PACKAGING.md) for build dependencies and distribution integration. Luciole needs a Wayland session with EGL/OpenGL ES support. Trash and network locations use GIO/GVfs and a session D-Bus connection; media and archive helpers are listed in the packaging guide.

Launch Luciole from your application menu, or open a folder from the terminal:

```sh
luciole
luciole ~/Downloads
```

For source builds, follow the [Meson build instructions](PACKAGING.md#build). Development commands and checks are covered in [CONTRIBUTING.md](CONTRIBUTING.md).

## Configuration

Open Settings with Ctrl+, or from the main menu. Changes save automatically and apply without restarting.

For manual configuration, use `~/.config/luciole/config.toml`. Preferences changed in the UI are stored separately in `settings.toml`. The [configuration guide](docs/user/configuration.md) explains paths, precedence and available preferences.

## Documentation

- [User guide](docs/user/index.md): browsing, selection, views, settings and file actions.
- [Design notes](docs/design/README.md): architecture and implementation contracts.
- [Packaging](PACKAGING.md): dependencies, runtime providers, installed files and distribution integration.
- [Contributing](CONTRIBUTING.md): development builds, code style and checks.

## Contributing

Bug reports, fixes and documentation improvements are welcome. Use the [issue tracker](https://github.com/noctalia-dev/luciole/issues) for bugs and feature requests, and [Discord](https://discord.noctalia.dev) for help and discussion. For bug reports, include your Luciole version, distribution and steps to reproduce the issue.

Read [CONTRIBUTING.md](CONTRIBUTING.md) before submitting changes.

## License

MIT License. See [LICENSE](LICENSE) for details.
