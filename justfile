set positional-arguments
set shell := ["bash", "-euo", "pipefail", "-c"]

mode := "debug"
prefix := "/usr/local"
cpp-std := "c++23"

default:
    @just --list

_check-mode m:
    case "$1" in debug|release|asan) ;; *) echo "error: expected debug, release, or asan" >&2; exit 1 ;; esac

configure m=mode install_prefix=prefix: (_check-mode m)
    #!/usr/bin/env bash
    set -euo pipefail
    args=(--buildtype=debug -Dcpp_std={{cpp-std}} -Dtests=auto)
    [[ "$1" == release ]] && args=(--buildtype=release -Dcpp_std={{cpp-std}} -Dtests=auto -Db_lto=true)
    [[ "$1" == asan ]] && args+=(-Db_sanitize=address,undefined)
    args+=(-Dpkg_config_path="${PKG_CONFIG_PATH-}")
    if [[ -f "build-$1/meson-private/coredata.dat" ]]; then
        meson setup "build-$1" "${args[@]}" --prefix "$2" --reconfigure
    else
        meson setup "build-$1" "${args[@]}" --prefix "$2"
    fi

_ensure-configured m=mode: (_check-mode m)
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-$1/build.ninja" ]]; then
        just configure "$1"
    else
        configured_pkg_path="$(meson introspect "build-$1" --buildoptions | python3 -c 'import json,sys; print(":".join(next(option["value"] for option in json.load(sys.stdin) if option["name"] == "pkg_config_path")))')"
        if [[ "$configured_pkg_path" != "${PKG_CONFIG_PATH-}" ]]; then
            meson configure "build-$1" -Dpkg_config_path="${PKG_CONFIG_PATH-}"
        fi
    fi

build m=mode: (_ensure-configured m)
    meson compile -C "build-$1" noctalia-files

run m=mode: (build m)
    NOCTALIA_FILES_ASSETS_DIR="$PWD/assets" ./build-$1/noctalia-files

test m=mode *args: (_ensure-configured m)
    meson configure "build-$1" -Dtests=enabled
    meson test -C "build-$1" --print-errorlogs "${@:2}"

install m: (_check-mode m)
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -x "build-$1/noctalia-files" ]]; then
        echo "error: build-$1/noctalia-files is missing; build it before installing" >&2
        exit 1
    fi
    meson install --no-rebuild -C "build-$1"

uninstall m: (_check-mode m)
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-$1/build.ninja" ]]; then
        echo "error: build-$1 is not configured" >&2
        exit 1
    fi
    ninja -C "build-$1" uninstall

_check_formatter:
    #!/usr/bin/env bash
    set -euo pipefail
    version="$(clang-format --version)"
    if [[ ! "$version" =~ version\ ([0-9]+)\. ]] || [[ "${BASH_REMATCH[1]}" -lt 22 ]]; then
        echo "error: formatting requires clang-format 22 or newer" >&2
        exit 1
    fi

format: format-code format-nix

format-check: format-code-check format-nix-check lint-prose

format-code: _check_formatter
    python3 tools/project_files.py code | xargs -0 -r clang-format -i

format-code-check: _check_formatter
    python3 tools/project_files.py code | xargs -0 -r clang-format --dry-run -Werror

format-nix:
    python3 tools/project_files.py nix | xargs -0 -r nixfmt

format-nix-check:
    python3 tools/project_files.py nix | xargs -0 -r nixfmt --check

_clang_tidy m=mode: (_ensure-configured m)
    meson compile -C "build-$1" noctalia-files
    run-clang-tidy -quiet -p "build-$1" -j "$(nproc)" -warnings-as-errors='*' -header-filter='(\.\./)?src/.*' "$(realpath src)/.*"

lint m=mode: (_clang_tidy m) lint-nix lint-python lint-prose

lint-nix:
    python3 tools/project_files.py nix | xargs -0 -r -n1 statix check
    python3 tools/project_files.py nix | xargs -0 -r deadnix --fail

lint-prose:
    python3 tools/lint_prose.py

lint-python:
    python3 tools/project_files.py python | xargs -0 -r python3 -m py_compile

fix m=mode: (_ensure-configured m)
    meson compile -C "build-$1" noctalia-files
    run-clang-tidy -quiet -p "build-$1" -j "$(nproc)" -fix -header-filter='(\.\./)?src/.*' "$(realpath src)/.*"
    just format-code

clean m=mode: (_check-mode m)
    rm -rf -- "build-$1"

rebuild m=mode: (clean m) (build m)
