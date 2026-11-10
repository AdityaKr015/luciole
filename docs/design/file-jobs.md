# File jobs and desktop boundaries

## Job ownership

[OperationHub](../../src/app/operation_hub.h) owns accepted file jobs independently of their initiating page. [JobRouting](../../src/browser/job_routing.h) selects the page that presents them. Follow-scope jobs can transfer ownership when a page closes; page-scoped preparation is cancelled unless explicitly promoted. Services perform work; page controllers compose dialogs and dispatch captured requests.

Keep captured destinations and target identities throughout preparation, conflict decisions and retries. A later pane focus, navigation, selection or row rebind must not redirect an operation. Provider completions also notify other relevant visible pages, including providers without native directory monitoring.

## Transfers, conflicts and partial outcomes

[TransferService](../../src/operations/transfer_service.h) reports per-source effects, cancellation, conflicts and retries. Native copy/publication helpers and [provider_transfer.cpp](../../src/operations/provider_transfer.cpp) implement different capability sets; do not advertise native guarantees for a provider that cannot supply them.

Native folder merging revalidates identities and scopes conflict decisions to the job. Replacing an entry requires the explicit conflict choice. Numbered naming preserves existing content. Cancellation stops remaining work and reports completed effects; it is not rollback. Cross-location moves must not delete sources whose destination copy failed. Retry only explicitly selected failed outcomes against revalidated inputs.

The [clipboard transport](../../src/clipboard/file_payload.h) and [desktop integration](../../src/platform/desktop_integration.h) preserve URI/raw-name semantics, action negotiation and one deletion owner across windows/clients. Native link drops and Ask negotiation reuse the existing transfer paths. Network operations require verified mounted/authenticated GIO providers; pending credential/connection UI is not an implicit capability.

Transfer/batch/creation/Trash undo, retained overwrite content and durable interrupted-job journals remain planned. Do not infer recoverability from a successful copy or temporary staging. Never silently resume deletion after a crash.

## Single-item rename history

[RenameReceipt](../../src/operations/rename_receipt.h) records a completed native rename with held parent/item descriptors and the resulting item revision. [RenameService](../../src/operations/rename_service.h) captures it on its worker and replays it through the same asynchronous job lifecycle. Reversal reopens the named parent, checks its pinned identity and both held/named item revisions, then uses `renameat2(RENAME_NOREPLACE)`. Raw names are preserved; symlinks are checked without following them. Revision comparisons include inode/type, size, mode, ownership, link count and nanosecond modification/change times, excluding access time.

The OperationHub owns one in-memory record and its Undo/Redo direction across pages/windows. Only successful captured mutations arm history. A mutation generation prevents older job completions from resurrecting invalidated history. Starting a new mutating job clears the prior record; preparation and read-only archive inspection do not. The same rule applies to retries. Replay uses an ordinary Follow-scope rename job, blocks new page operation actions while running and participates in application shutdown/draining. Collision/access errors retain a retryable record; changed/missing items or a replaced parent retire it. If a rename commits but capturing its next receipt fails, report the completed rename without arming another inverse.

This first scope is single-item native Rename only. It does not overwrite, save deleted content, traverse directories, persist a journal or claim atomic exclusion of a hostile writer between final source validation and rename. Keyboard routing leaves input undo/redo with the focused editor. [Rename-service tests](../../tests/rename_service_test.cpp) cover actual reversible filesystem effects and refusal cases; application-wide history and input routing also require real isolated-window checks.

## Other operations

Trash, selected deletion and restoration use explicit identity/preparation contracts in [trash_service.h](../../src/operations/trash_service.h), [restore_service.h](../../src/operations/restore_service.h) and [empty_trash_service.h](../../src/operations/empty_trash_service.h). Trash failure never falls through to permanent deletion. Recursive permission jobs and folder totals require explicit scope; selection alone starts neither traversal.

[ArchiveService](../../src/operations/archive_service.h) owns bounded read-only catalogs, extraction/creation and temporary member-open leases. Validate archive paths and link semantics before staging; publication uses transfer conflicts/outcomes. System libarchive is the main backend, with a bounded system 7-Zip CLI for encrypted 7z input. Keep passwords in per-job memory, never persisted configuration. [PACKAGING.md](../../PACKAGING.md) owns provider requirements and compatibility limits.

## Opening and execution

[FileLauncher](../../src/platform/file_launcher.h) uses asynchronous GIO associations. [ProgramLauncher](../../src/platform/program_launcher.h) provides separate explicit native execution, captured desktop-launcher confirmation and trusted user Scripts actions. The [execution controller](../../src/app/page/execution_controller.h) gates results by captured context, cancellation and page lifetime. Selected paths are argv entries, never shell interpolation.

Desktop confirmation displays the captured name/Exec and launches the captured key file after revalidation. Native execution preserves original pathname/script argv semantics, so it does not eliminate the check-to-exec race against hostile concurrent writers. Scripts are deliberately installed programs, not sandboxed extensions. Menu dismissal cancels the confirmation's captured token, not the new accepted launch token. Fresh activation is scoped to the originating surface; inherited startup tokens are cleared. Handoff success does not mean application readiness or successful exit.

## Verification

Use temporary fixtures for races, conflicts, raw/Unicode names, partial moves, source retention and cancellation. Tests are declared in [tests/meson.build](../../tests/meson.build), including transfer/provider-transfer, archive, Trash/restore, clipboard/payload, job-routing and launcher suites. Real GTK/Qt interoperability, private network servers, compositor input and crash/failure injection complement headless tests. Preserve the exact boundaries of previous results in [AGENTS.md](../../AGENTS.md) and outstanding coverage in [PLAN.md](../../PLAN.md#release-verification-and-open-follow-ups).
