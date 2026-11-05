# Contributing

noctalia-files shares its conventions with [noctalia](https://github.com/noctalia-dev/noctalia) and umbriel: same team, same style. If in doubt, match what noctalia does.

## Code Style

Run `just format` before committing and `just format-check` to check. Code formatting requires [clang-format](https://clang.llvm.org/docs/ClangFormat.html) 23 or newer and uses the same `.clang-format` as noctalia; the recipes print the version in use. Keep editor formatting on the same version as your command-line formatter.

`just configure` creates a root `compile_commands.json` symlink to the selected Meson build directory, so clangd and clang-tidy see the build you are working in.

The repo includes `lefthook.yml`. Run `lefthook install` to install the pre-commit hook; it runs `just format-code` when C++ files are staged and refreshes the git index for tracked formatting changes.

Owned text files must not contain em dashes or trailing whitespace and must end with a newline; `just lint-prose` checks this.

### Lint

Code must pass clang-tidy with warnings treated as errors, using the same `.clang-tidy` check set as noctalia. `just lint` checks the whole tree; while iterating, lint only the `.cpp` files you touched:

```sh
clang-tidy -p build-debug --warnings-as-errors='*' <touched .cpp files...>
```

Headers are linted through the `.cpp` files that include them. Fix every finding before opening a pull request. Prefer the modern idioms the checks enforce: `auto`, ranges, `std::format`/`std::print`, `make_unique`, scoped locks, no C-style casts, uppercase literal suffixes (`1.0F`).

### Naming Conventions

|                        | Convention           | Example             |
| ---------------------- | -------------------- | ------------------- |
| Files                  | snake_case           | `directory_model.h` |
| Directories            | snake_case           | `browser/`          |
| Types / Classes        | PascalCase           | `DirectoryModel`    |
| Functions / Methods    | camelCase            | `selectAll()`       |
| Variables / Parameters | camelCase            | `scrollAnchor`      |
| Private members        | m_camelCase          | `m_selection`       |
| Enumerators            | PascalCase           | `SortField::Name`   |
| Macros                 | SCREAMING_SNAKE_CASE | `TEST_CHECK`        |

Scoped enums (`enum class`) use an explicit `std::uint8_t` underlying type where it makes sense. Getters are the noun, without a `get` prefix: `snapshot()`, `selection()`.

### Source Conventions

- `src/` is the include root; project includes are relative to it (`#include "app/application.h"`).
- Headers use `#pragma once` and sit beside their sources; there is no separate `include/` tree.
- Brace the body of every `if`, `else`, `for`, and `while`, including single statements.
- Use `[[nodiscard]]` only where ignoring the result is likely a bug: a call with side effects that reports failure or hands back something the caller must own, consume or keep (a handle, token or id), and `&&` builders. Mark result and status types with `struct [[nodiscard]] Name` instead of each function that returns them. Getters and pure functions go without it.

### Comments

Comments explain what the current code does and any non-obvious constraint a reader needs. Do not narrate history, rejected alternatives, or "why we don't do X"; git holds that. If a comment is longer than the code it describes, cut it down.

### Code shared with noctalia-shell

Large parts of `src/` (UI controls, render, Wayland surfaces, core helpers) are copied from [noctalia-shell](https://github.com/noctalia-dev/noctalia). Make generic fixes there first and resync the copy, so the two trees do not drift apart.

## Testing

Write test checks with `TEST_CHECK` and `TEST_NEAR` from `tests/test_check.h`, never `assert` or a per-file helper. `assert` compiles away under `NDEBUG`, so a release build would pass a broken test silently; `TEST_CHECK` prints the failed expression with its file and line and exits non-zero. The header refuses to compile with `NDEBUG`, so test executables set `override_options: ['b_ndebug=false']` in `meson.build`. Wrap an expression that contains a top-level comma in an extra pair of parentheses: `TEST_CHECK((ids == std::vector<int>{1, 2}))`.

## Commit Messages

Use [Conventional Commits](https://www.conventionalcommits.org/): `type(scope): summary`, or `type: summary` when no scope helps, with lowercase types such as `feat`, `fix`, `refactor`, `chore`, `test`, or `docs`. Keep the summary concise, imperative, and specific.

Default to the summary line alone. Add a body only when it carries information a reader would otherwise miss: a non-obvious root cause, a constraint that forced the approach, a behavior change users will notice, or a `Fixes #N` reference. Do not restate the diff or list touched files.
