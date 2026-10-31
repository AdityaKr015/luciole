"""Exercise real desktop dispatch with private associations and a recording handler."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def run_case(binary, mode, names):
    with tempfile.TemporaryDirectory(prefix="noctalia-launch-test-") as directory:
        root = Path(directory)
        for name in ("home", "config", "data/applications", "cache", "state", "runtime"):
            (root / name).mkdir(parents=True)
        (root / "runtime").chmod(0o700)
        for data_dir in os.environ.get("XDG_DATA_DIRS", "/usr/local/share:/usr/share").split(":"):
            mime = Path(data_dir) / "mime"
            if (mime / "mime.cache").is_file():
                (root / "data/mime").symlink_to(mime)
                break
        else:
            raise RuntimeError("shared MIME database prerequisite unavailable")
        handler = root / "handler.py"
        log = root / "launched.jsonl"
        handler.write_text(
            "import json,os,sys\nfrom pathlib import Path\n"
            f"with Path({str(log)!r}).open('a') as stream:\n"
            " stream.write(json.dumps([os.fsencode(arg).hex() for arg in sys.argv[1:]])+'\\n')\n"
        )
        desktop = root / "data/applications/fixture.desktop"
        desktop.write_text(
            "[Desktop Entry]\nType=Application\nName=Private launch fixture\n"
            f"Exec={sys.executable} {handler} %U\n"
            "MimeType=text/plain;\n" + ("NoDisplay=false\n" if mode == "with" else "NoDisplay=true\n")
        )
        (root / "config/mimeapps.list").write_text(
            "[Default Applications]\ntext/plain=fixture.desktop;\n"
        )
        (root / "data/applications/mimeinfo.cache").write_text("[MIME Cache]\ntext/plain=fixture.desktop;\n")
        guard = root / "exec-guard.py"
        refused = root / "unexpected-exec"
        guard.write_text(
            f"#!{sys.executable}\nimport os,sys\nfrom pathlib import Path\n"
            f"if sys.argv[1:3] != {[sys.executable, str(handler)]!r}:\n"
            f" Path({str(refused)!r}).write_text('unexpected desktop handler')\n"
            " sys.exit(1)\n"
            "os.execv(sys.argv[1],sys.argv[1:])\n"
        )
        guard.chmod(0o700)
        paths = []
        for name in names:
            path = os.fsencode(root) + b"/" + name
            if mode != "error":
                with open(path, "wb") as stream:
                    stream.write(b"private fixture\n")
            paths.append(path)
        env = dict(os.environ)
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "GIO_LAUNCH_DESKTOP"):
            env.pop(key, None)
        env.update(
            HOME=str(root / "home"),
            XDG_CONFIG_HOME=str(root / "config"),
            XDG_CONFIG_DIRS=str(root / "no-system-config"),
            XDG_DATA_HOME=str(root / "data"),
            XDG_DATA_DIRS=str(root / "no-system-data"),
            XDG_CACHE_HOME=str(root / "cache"),
            XDG_STATE_HOME=str(root / "state"),
            XDG_RUNTIME_DIR=str(root / "runtime"),
            XDG_CURRENT_DESKTOP="",
            DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(root / "no-session-bus"),
            DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + str(root / "no-system-bus"),
            GIO_LAUNCH_DESKTOP=str(guard),
            GIO_USE_VFS="local",
            GIO_USE_PORTALS="0",
            NOCTALIA_FILES_LOG_LEVEL="debug",
            G_DEBUG="fatal-warnings",
        )
        result = subprocess.run(
            [binary, mode, *paths], env=env, capture_output=True, timeout=10, check=False
        )
        if result.returncode != 0:
            raise AssertionError((result.returncode, result.stdout.decode(errors="replace"), result.stderr.decode(errors="replace")))
        if mode in ("open", "sequential", "with"):
            deadline = time.monotonic() + 5
            expected = sorted(path.hex() for path in paths)
            while True:
                try:
                    actual = sorted(
                        item
                        for line in log.read_text().splitlines()
                        for item in json.loads(line)
                    )
                except (FileNotFoundError, json.JSONDecodeError):
                    actual = []
                if actual == expected:
                    break
                if time.monotonic() >= deadline:
                    raise AssertionError((actual, expected, result.stderr))
                time.sleep(0.01)
        if mode == "error" and log.exists():
            raise AssertionError("a missing file launched an application")
        if refused.exists():
            raise AssertionError("desktop dispatch escaped its fixture")
        # Debug diagnostics identify stages and requests, without recording file payloads.
        diagnostics = result.stderr.decode(errors="replace")
        if "GIO completion" not in diagnostics:
            raise AssertionError("missing launch timing diagnostics")
        if mode in ("open", "sequential") and ("handler=fixture.desktop" not in diagnostics or "desktop handoff" not in diagnostics):
            raise AssertionError("missing desktop handoff timing")
        if str(root) in diagnostics:
            raise AssertionError("launch diagnostics disclosed the file location")


def main():
    binary = sys.argv[1]
    run_case(binary, "open", [b"plain.txt", "space ' $() <literal> café\n.txt".encode(), b"raw-\xff.txt"])
    run_case(binary, "sequential", [b"first.txt", b"second.txt", b"third.txt"])
    run_case(binary, "with", [b"explicit.txt", "space ' $() café\n.txt".encode(), b"raw-\xff.txt"])
    run_case(binary, "error", [b"missing.txt"])
    run_case(binary, "destroy", [b"cancelled.txt"])
    print("file launcher desktop integration checks passed")


if __name__ == "__main__":
    main()
