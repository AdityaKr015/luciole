# Configuration and persistence

The [user configuration guide](../user/configuration.md) owns the preference reference. The implementation is [settings.h](../../src/config/settings.h), [settings.cpp](../../src/config/settings.cpp) and [ViewPreferenceWriter](../../src/config/view_preference_writer.h).

## Ownership and reload

Built-in defaults are overlaid by the optional base `config.toml`, then writable GUI `settings.toml`. GUI edits never modify the base. Canonical names have one spelling; unknown keys, wrong types and unsupported values fail validation. Schema version is integer 1 when supplied. The validation CLI checks one required file, with no adjacent overrides and no compositor initialization.

The worker serializes typed edits and reload requests, coalescing pending edits per preference. Saves reread and validate actual persisted inputs before merging only the requested keys. Removing an override restores inheritance. An invalid reload keeps the last valid snapshot; failed saves reload actual persisted state. Publish only after pending edits/reloads drain.

File monitors follow canonical files, existing path components and resolved declarative targets. Coalesced events trigger worker reads; the read after watch installation closes the read/install gap. Callbacks publish during Update. Only relevant global changes should replace a live preference; an unrelated reload must not reset the current visit's view/zoom/history state. Ordinary close drains pending preference saves.

## Atomic writes

[atomic_file.cpp](../../src/fs/atomic_file.cpp) implements anchored local transactions. Cooperating writers use a persistent advisory lock. Preserve that lock across ordinary cleanup; unlinking it can split writer coordination. New private directories use 0700 and data/temporary/lock files use 0600.

Writes use an exclusive neighboring temporary, file flush, destination revision recheck, atomic publication and parent-directory flush. First creation must not replace an unexpected destination. Reject unsafe sidecar targets, including symlink/hard-link, foreign-owned, special and read-only files. Declarative base symlinks remain readable under the loader's separate rules.

Outcomes distinguish unchanged, not committed, committed but directory synchronization failed, and durable. Report visible-but-not-durable commits accurately. Cleanup removes only the transaction's own temporary. This protects cooperating writers and ordinary races; it does not promise atomic exclusion of an uncooperative same-user writer between a final check and rename.

## State and caches

[StateStore](../../src/config/state_store.h) owns a separate versioned Places schema and persistence revision. It is not a complete durable tab/window session. Do not conflate settings, Places state, future operation journals and regenerable thumbnails. Credentials do not belong in configuration, state, URI history or cache identities.

The shared thumbnail cache and Luciole' ownership ledger are regenerable. Pruning must preserve untracked entries and foreign replacements. A future operation journal has different ordering/recovery requirements; cache persistence is not an implementation of crash-safe file-job recovery.

Relevant suites are [settings](../../tests/settings_test.cpp), [preference writer](../../tests/view_preference_writer_test.cpp), [atomic files](../../tests/atomic_file_test.cpp) and [state store](../../tests/state_store_test.cpp). Use temporary XDG roots and actual invalid/replaced inputs. Test automatic reload and settings layout in an isolated window when those behaviors change.
