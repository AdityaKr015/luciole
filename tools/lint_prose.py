"""Check owned source and guidance for the project's punctuation and whitespace rules."""

import sys

from project_files import project_files

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
