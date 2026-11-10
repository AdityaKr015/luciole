# Architecture and ownership

## Module boundaries

[meson.build](../../meson.build) assembles the local Noctalia library and Files-owned static modules. [CONTRIBUTING.md](../../CONTRIBUTING.md#module-layering) defines the allowed dependencies. Public domain APIs use `Location`, typed metadata and `CancelToken`; UI callers do not operate directly on GIO file objects.

| Area | Responsibility |
| --- | --- |
| `lib/` | Mirrored renderer, text, scene/input, shared controls, generic Wayland transport and image texture residency |
| `src/model/` | Plain browsing/thumbnail options and shared domain types |
| `src/fs/` | URI-backed locations, filesystem queries, directory sessions, errors and anchored local persistence |
| `src/config/` | Validated settings, GUI overrides and separate versioned Places state |
| `src/browser/` | Published directory model, controllers, selection, history, Miller chains and tab/job routing |
| `src/operations/` | Worker-backed file jobs, conflicts, per-item outcomes and archive services |
| `src/imaging/`, `src/system/`, `src/thumbnails/` | Decoders, artwork resolution, generation providers, metadata and disk cache |
| `src/platform/`, `src/clipboard/` | Files-owned desktop launching, decoration, clipboard and native drop transport |
| `src/ui/` | Files-specific views and compositions of actual Noctalia controls |
| `src/app/`, `src/event_loop/` | Application ownership, page orchestration, platform polling and GLib integration |

The application uses direct Wayland and OpenGL ES with Pango/Cairo. GdkPixbuf/Glycin supplies image decoding, not a widget layer. Builds consume the checked-in mirror; a sibling checkout and the Noctalia shell are not runtime/build requirements. Generic library changes go upstream before reviewed updates are copied back into Files.

## Windows, tabs and pages

[WorkspaceApplication](../../src/app/workspace_application.h) owns native windows, the workspace, application-wide preferences and caches, and the [OperationHub](../../src/app/operation_hub.h). [TabWorkspace](../../src/browser/tab_workspace.h) represents logical tabs and optional splits. A native host supplies the visible toolbar/sidebar and mounts the active page roots.

[BrowserPage](../../src/app/page/browser_page.h) orchestrates feature controllers rather than implementing file jobs or decoder work. Each page has its own browse controller, selection, history, query and viewport. Its [MillerColumns](../../src/browser/miller_columns.h) chain owns additional lazy controllers within the same page. The focused controller supplies action and preview targets. Offscreen/retired branches release resources; at most 16 live columns remain while deeper navigation retires old ancestors.

Page visibility does not confer ownership of running file jobs. Accepted Follow-scope jobs can move to another page when a tab closes; page-scoped preparation is cancelled. The OperationHub also owns one in-memory native rename Undo/Redo record across pages and windows. See [file jobs](file-jobs.md). Current tab/window state is in memory; durable session restoration remains unfinished.

## Asynchronous publication

[DirectorySession](../../src/fs/directory_session.h) enumerates asynchronously through GIO and publishes typed snapshots. [DirectoryModel](../../src/browser/directory_model.h) owns the current projection. Operation names and escaped URIs remain separate from display text; sorting or Unicode search must not rewrite operation identities.

Selections, focus and anchors use stable entry IDs, not row indices. Refresh preserves surviving identities; deletion/replacement retires them. Controllers reject old generations. A pointer release must not activate a different entry after a virtual row is rebound.

The main loop integrates the default GLib context through [GlibPollSource](../../src/event_loop/glib_poll_source.h), alongside the shared Wayland and deferred-call sources. Workers perform filesystem traversal, decoding, provider work and configuration reads/writes. Completion callbacks verify cancellation, captured context and owner lifetime, then request Update/Layout/Redraw. Rendering must not perform I/O or rebuild the scene.

Relevant regressions include [directory sessions](../../tests/directory_session_test.cpp), [models](../../tests/directory_model_test.cpp), [Miller columns](../../tests/miller_columns_test.cpp), [tab workspace](../../tests/tab_workspace_test.cpp), [job routing](../../tests/job_routing_test.cpp) and [input lifetime](../../tests/input_lifetime_test.cpp). GUI ownership and compositor behavior also need isolated real-window checks; headless contracts alone do not establish them.
