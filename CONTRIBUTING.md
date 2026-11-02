# Contributing

## Code Style

Run `just format` before committing and `just format-check` to check.

### Source Conventions

- `src/` is the include root; project includes are relative to it (`#include "app/application.h"`).
- Use `[[nodiscard]]` only where ignoring the result is likely a bug: a call with side effects that reports failure or hands back something the caller must own, consume or keep (a handle, token or id), and `&&` builders. Mark result and status types with `struct [[nodiscard]] Name` instead of each function that returns them. Getters and pure functions go without it.

### Code shared with noctalia-shell

Large parts of `src/` (UI controls, render, Wayland surfaces, core helpers) are copied from [noctalia-shell](https://github.com/noctalia-dev/noctalia). Make generic fixes there first and resync the copy, so the two trees do not drift apart.
