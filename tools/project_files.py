"""Enumerate owned files without consulting repository metadata."""

import os
import sys
from pathlib import Path

EXCLUDED = {
    ".git",
    ".agents",
    ".aws",
    ".codex",
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
        sys.exit("usage: project_files.py <code|nix|python|text>")
    for item in project_files(sys.argv[1]):
        sys.stdout.buffer.write(os.fsencode(item) + b"\0")
