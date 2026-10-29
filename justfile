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

_files kind:
    #!/usr/bin/env python3
    """Enumerate owned files without consulting repository metadata."""

    import os
    import sys
    from pathlib import Path

    EXCLUDED = {
        ".git",
        ".aws",
        ".cache",
        ".direnv",
        ".idea",
        ".vscode",
        "__pycache__",
        "node_modules",
        "result",
    }

    EXTENSIONS = {
        "code": {".cpp", ".h"},
        "nix": {".nix"},
        "python": {".py"},
        "text": {".md", ".mdx", ".cpp", ".h", ".in", ".py", ".nix", ".txt", ".build", ".toml", ".bash", ".fish"},
    }


    def project_files(kind):
        for directory, children, names in os.walk(".", followlinks=False):
            children[:] = sorted(
                name
                for name in children
                if name not in EXCLUDED
                and not name.startswith(("build-", "result-"))
                and not Path(directory, name).is_symlink()
            )
            for name in sorted(names):
                path = Path(directory, name)
                if path.is_symlink():
                    continue
                if path.suffix in EXTENSIONS[kind] or (
                    kind == "text"
                    and name in ("justfile", "VERSION", "LICENSE", ".clang-format", ".clang-tidy", "_noctalia-files")
                ):
                    yield path


    if __name__ == "__main__":
        if len(sys.argv) != 2 or sys.argv[1] not in ("code", "nix", "python", "text"):
            sys.exit("usage: just _files <code|nix|python|text>")
        for item in project_files(sys.argv[1]):
            sys.stdout.buffer.write(os.fsencode(item) + b"\0")

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
    just _files code | xargs -0 -r clang-format -i

format-code-check: _check_formatter
    just _files code | xargs -0 -r clang-format --dry-run -Werror

format-nix:
    just _files nix | xargs -0 -r nixfmt

format-nix-check:
    just _files nix | xargs -0 -r nixfmt --check

_clang_tidy m=mode: (_ensure-configured m)
    meson compile -C "build-$1" noctalia-files
    run-clang-tidy -quiet -p "build-$1" -j "$(nproc)" -warnings-as-errors='*' -header-filter='(\.\./)?src/.*' "$(realpath src)/.*"

lint m=mode: (_clang_tidy m) lint-nix lint-python lint-prose

lint-nix:
    just _files nix | xargs -0 -r -n1 statix check
    just _files nix | xargs -0 -r deadnix --fail

lint-prose:
    #!/usr/bin/env python3
    """Check owned source and guidance for the project's punctuation and whitespace rules."""

    import os
    import subprocess
    import sys
    from pathlib import Path

    def project_files(kind):
        paths = subprocess.check_output(["just", "_files", kind]).split(b"\0")
        return (Path(os.fsdecode(path)) for path in paths if path)

    errors = []
    for path in project_files("text"):
        text = path.read_text(encoding="utf-8")
        if not text.endswith("\n"):
            errors.append(f"{path}: missing final newline")
        for number, line in enumerate(text.splitlines(), 1):
            if "\u2014" in line:
                errors.append(f"{path}:{number}: em dashes are prohibited")
            if line != line.rstrip():
                errors.append(f"{path}:{number}: trailing whitespace")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        sys.exit(1)

lint-python:
    just _files python | xargs -0 -r python3 -m py_compile

fix m=mode: (_ensure-configured m)
    meson compile -C "build-$1" noctalia-files
    run-clang-tidy -quiet -p "build-$1" -j "$(nproc)" -fix -header-filter='(\.\./)?src/.*' "$(realpath src)/.*"
    just format-code

clean m=mode: (_check-mode m)
    rm -rf -- "build-$1"

rebuild m=mode: (clean m) (build m)
