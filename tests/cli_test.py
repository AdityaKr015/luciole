"""Exercise the public CLI without a graphical session."""

import os
import subprocess
import sys
import unittest
from pathlib import Path

BINARY = str(Path(sys.argv.pop(1)).resolve())
VERSION = sys.argv.pop(1)


class CliTest(unittest.TestCase):
    def invoke(self, *args):
        env = dict(os.environ)
        for name in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"):
            env.pop(name, None)
        return subprocess.run([BINARY, *args], check=False, capture_output=True, text=True, env=env, timeout=5)

    def test_headless_information(self):
        version = self.invoke("--version")
        self.assertEqual(version.returncode, 0, version.stderr)
        self.assertTrue(version.stdout.startswith(f"noctalia-files {VERSION} ("))
        self.assertEqual(version.stderr, "")
        help_result = self.invoke("--help")
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        self.assertIn("Usage:", help_result.stdout)
        self.assertEqual(help_result.stderr, "")

    def test_reject_invalid_argument_combinations(self):
        for args in (
            ("--unknown",),
            ("--help", "--version"),
            ("--version", "extra"),
            ("completions",),
            ("completions", "invalid"),
            ("completions", "bash", "extra"),
        ):
            with self.subTest(args=args):
                result = self.invoke(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
                self.assertTrue(result.stderr.startswith("noctalia-files:"))

    def test_completions_are_shell_specific(self):
        for shell, marker in (
            ("bash", "complete -F"),
            ("zsh", "#compdef noctalia-files"),
            ("fish", "complete -c noctalia-files"),
        ):
            with self.subTest(shell=shell):
                result = self.invoke("completions", shell)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(marker, result.stdout)
                self.assertEqual(result.stderr, "")

    def test_startup_reports_unimplemented_browser(self):
        result = self.invoke()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("graphical browser is not implemented", result.stderr)


if __name__ == "__main__":
    unittest.main()
