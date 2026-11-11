# File operations

## Copy, move and create

Use Ctrl+C or Ctrl+X for the selection and Ctrl+V in the destination folder. Paste is also available from context menus, including List view. Clipboard transfers and drag/drop work between Luciole windows; the same transport supports other file managers.

Drop files on a folder or the destination pane. Ctrl requests Copy, Shift requests Move and Alt offers an action chooser. Symbolic-link drops are supported for native locations. Dragging a folder into Places pins it instead of transferring its contents.

When names collide, the conflict dialog offers Replace, Skip and a numbered new name. Folder copies can merge into existing folders. Apply-to-all choices are scoped to the current job. Failed-item retries are explicit. Cancellation stops remaining work; it does not undo already completed changes. Supported native copies, links and moves have [Undo/Redo](#undo-and-redo); interrupted-job recovery remains pending.

The background menu provides New Folder (Ctrl+Shift+N) and New Document. New Document offers an empty file and files from the configured XDG Templates folder. Create Link on a single native item creates a symbolic or hard link where supported.

F2 renames one item. Multiple selected native items from the same containing folder open batch rename. Preview the proposed names before applying them.

## File operations popover

Open File Operations from the toolbar or main menu to see jobs across tabs and windows in this running Luciole process. It shows preparation, transferred bytes, known item totals, pending decisions and results. Work with no known final total uses a spinner rather than a percentage. Pause is not offered because the current services cannot pause.

Cancel stops remaining work; completed effects remain. Closing the popover does not cancel accepted jobs. It dismisses when the window resizes; open it again from the toolbar at the new size. Review returns to the page handling a pending decision. Details shows a bounded report with Copy report. Retry failed is offered only while that transfer's retry information is available, and rechecks captured inputs without repeating completed items. Retried cut moves preserve the existing clipboard ownership checks.

The most recent 12 results stay in memory until Clear finished or application exit. Clearing results does not cancel running jobs. Older reports can outlive their service, so Retry may no longer be available. This is progress and reporting, not a persistent recovery journal. Separate Luciole processes have separate job lists.

## Undo and redo

Undo (Ctrl+Z) reverses the most recent supported native creation, rename, copy or move. Redo (Ctrl+Shift+Z) reapplies it. The main menu and folder background menu name the action, such as Undo Create Folder, Undo Batch Rename, Undo Copy, Undo Create Links or Undo Move. A fully completed batch rename or supported move selection is one action. The one-operation history is shared by tabs and windows in the same running Luciole process; it does not survive restart or cross independent processes. Text inputs retain their own text undo/redo.

Undo creation removes the unchanged new folder, empty file, template document or link from its containing folder. It does not send it to Trash. Changed, replaced or missing items are refused, and folders must still be empty. Creation Undo never traverses or deletes folder contents. Undoing a link removes only the new link. Redo uses the original created content, so changing or removing a document template does not change the restored document. Hard-link Redo requires its retained target to remain available and unchanged.

An unrelated occupant blocks reversal; move it aside before retrying. Reversal checks containing-folder and item identities/revisions. Reading a file does not invalidate the record. Batch replay checks all affected items and destinations before changing names and handles swaps and cycles. Entries whose names did not change are excluded.

Undo Move returns files, folders and links to their original containing folders. Redo returns them to the actual destination names, including numbered names chosen with Keep Both. A fully completed same-filesystem native selection is one action, including supported replacements and folder merges; skipped roots are excluded. Reversal checks all captured items and return names first. An unrelated occupant blocks reversal without being overwritten. Cross-filesystem/provider moves, partial and retried jobs do not supply move history. Large selections, unreadable snapshots or insufficient descriptor headroom can also leave a successful move without Undo.

For a move replacement, Undo returns the incoming item to its source and restores the actual previous destination. Redo retains that previous destination again and moves the incoming item back. The previous version must remain unchanged, including its complete folder contents. Plain moved folders travel with their contents without traversal or deletion; nested edits travel with that folder when its root revision remains unchanged.

For a move-folder merge, Undo restores the original source-folder inodes and returns only the transferred children, including children whose previous destinations were replaced. Destination-only files and later sibling additions stay in place. Empty merged source folders are retained privately until Undo restores them; even an empty move merge can be undone. Redo validates the restored source trees and refuses edited or added children before moving anything. It retains each source container only after its recorded children leave and it is empty. Existing destination folders remain, and normal directory modification times can change.

Undo Copy removes the unchanged copied destinations from view, leaving the originals untouched. Undo Create Links removes only the new symbolic links. A completed native copy/link selection is one action, including numbered Keep Both names, folders, hidden children and dangling links. Completed native copy merges and supported replacements participate in that action; partial or retried jobs and provider transfers do not supply history. The recorded tree must remain unchanged; edits, new children, replaced items or changed containing folders are refused before any root is moved. This check includes files nested inside copied folders.

Replacing a destination during Copy or a symbolic-link transfer can also be undone. Undo restores the actual previous file, folder or link; Redo restores the incoming version. Both versions must remain unchanged, including folder contents. Replay exchanges only the recorded versions, so an unrelated replacement at that name is refused. Reading either version does not invalidate history. Previous entries with hard-link aliases, unsupported types or unreadable/over-budget trees currently prevent replacement history; ordinary Replace still works without promising Undo. A batch that writes the same destination repeatedly currently keeps its previous versions for inspection instead of offering Undo.

For copy-folder merges, Undo reverses only the transferred children and restores any replaced children. Destination-only entries and later sibling additions remain. Existing merge folders are kept rather than replaced; normal filesystem modification times can change as entries are added or removed. An empty merge alone creates no history record. Move merges additionally restore their emptied source containers as described above.

Inactive versions are retained in owner-only `.luciole-create-*` folders beside their destinations: previous destinations while Undo is available, incoming copies while Redo is available. Move merges also retain emptied source containers inside the existing destination merge folders. Redo returns those same items without recopying from the source, preserving their content and metadata even if the original changed or disappeared. Unrecorded occupants are refused; copy/link replacement replay exchanges its two verified versions, while move replay restores the source and previous destination separately. Retention continues using the copied items' disk space; discarding history or clean exit removes only verified owned data on a worker. Incomplete reversal or changed retained contents preserve the remaining items for inspection, with paths in the operation/cleanup reports. Do not treat these private folders as regenerable cache or delete their contents blindly. After a crash, history is unavailable and private retained items may need manual recovery; no automatic deletion/resume is performed.

Copy history is optional above 4096 changed roots, 16384 captured entries counting both replacement versions, 64 levels per stored subtree or available descriptor headroom. Move history counts both moved roots and merged source containers toward its 4096-effect cap; replacement snapshots and restored merge-tree capture are bounded too. The ordinary copy still completes when optional history cannot be captured. Unreadable copied folders can also prevent history capture. Ordinary selection never starts a history traversal; capture and validation run only for accepted jobs.

Starting another mutating file job clears the previous history, including redo. Preparation-only dialogs and browsing do not clear it. A successful supported operation supplies a replacement record when its history can be captured; a no-op rename supplies none. History is unavailable while file jobs run. Partial operations do not arm history. Cancellation or an intervening filesystem change during replay can leave completed effects or retained items needing attention: inspect the File Operations report. Incomplete replay retires history when its captured proof is no longer valid.

An undone created document's original data can continue using disk space while its Redo record is retained. Clearing that history or exiting releases it. Trash, permission changes, unsupported transfer effects and a persistent recovery journal remain outside the current Undo/Redo support.

## Trash

Delete moves native selections to Trash. A failed trash operation does not silently become permanent deletion. Trash requires the system's GIO/GVfs provider.

In Trash, Restore returns selected items to their recorded original locations and asks about conflicts. Delete Permanently removes selected trashed items after confirmation. Empty Trash is available from the Trash sidebar row's context menu; its confirmation includes the measured contents and size. Permanent deletion is irreversible.

## Archives

Compress creates ZIP, TAR and 7z archives for native selections. Supported compressed TAR variants depend on the installed libarchive build and include gzip, bzip2, XZ and Zstandard. Browse Archive opens a read-only archive page; Extract Here and Extract To publish extracted contents through the normal transfer/conflict flow. Archive browsing can extract selected members or open an individual file through its associated application.

ZIP/TAR/7z/RAR input depends on available archive capabilities. Encrypted 7z input additionally uses a system 7-Zip executable. Passwords are requested when needed and are not stored in settings. Encrypted RAR is not supported. Numbered byte-split ZIP sets use `name.zip.001`, `.002` and so on; select `.001`, with all contiguous parts beside it. Spanned `.z01` sets and split-archive creation are not supported.

Opening and extracting archives validate members and enforce resource limits. An archive rejected as unsafe or unsupported is reported; Luciole does not silently disable these checks. Native archives are the current scope.

## Properties and permissions

Properties (Alt+Return) shows available file/media facts. Images can include pixel dimensions, videos can include codecs and duration, and PDFs can include page information. Installed providers determine which facts are available.

For folders, Properties starts a separate cancellable recursive count/size calculation. Ordinary selection and the side preview do not traverse folders to calculate totals. Recursive totals are currently unavailable for non-native locations and Trash.

Native permission and ownership editing is available in Properties. Multiple selections support bulk changes; recursive changes are explicit operations. Advanced permissions opens the POSIX ACL editor on supported native filesystems. Merely selecting a folder does not apply anything recursively. Luciole reports unsupported or denied changes rather than claiming they succeeded.

## Network locations

Mounted, authenticated SFTP, SMB and WebDAV locations support the verified copy/move paths through installed GIO/GVfs backends. Some providers cannot preserve native symlinks, raw filename bytes or every metadata field. Read-only targets, conflicts, disconnects and partial moves remain explicit outcomes. Luciole does not provide its own server connection or credentials dialog yet.

See [runtime providers](../../PACKAGING.md#gvfs-providers-across-distributions) for distribution package requirements and [the remaining plan](../../PLAN.md) for unfinished device/network integration.
