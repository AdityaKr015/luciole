# File operations

## Copy, move and create

Use Ctrl+C or Ctrl+X for the selection and Ctrl+V in the destination folder. Paste is also available from context menus, including List view. Clipboard transfers and drag/drop work between Luciole windows; the same transport supports other file managers.

Drop files on a folder or the destination pane. Ctrl requests Copy, Shift requests Move and Alt offers an action chooser. Symbolic-link drops are supported for native locations. Dragging a folder into Places pins it instead of transferring its contents.

When names collide, the conflict dialog offers Replace, Skip and a numbered new name. Folder copies can merge into existing folders. Apply-to-all choices are scoped to the current job. Failed-item retries are explicit. Cancellation stops remaining work; it does not undo already completed changes. Transfer undo/redo and interrupted-job recovery are not implemented yet.

The background menu provides New Folder (Ctrl+Shift+N) and New Document. New Document offers an empty file and files from the configured XDG Templates folder. Create Link on a single native item creates a symbolic or hard link where supported.

F2 renames one item. Multiple selected native items from the same containing folder open batch rename. Preview the proposed names before applying them.

## File operations popover

Open File Operations from the toolbar or main menu to see jobs across tabs and windows in this running Luciole process. It shows preparation, transferred bytes, known item totals, pending decisions and results. Work with no known final total uses a spinner rather than a percentage. Pause is not offered because the current services cannot pause.

Cancel stops remaining work; completed effects remain. Closing the popover does not cancel accepted jobs. It dismisses when the window resizes; open it again from the toolbar at the new size. Review returns to the page handling a pending decision. Details shows a bounded report with Copy report. Retry failed is offered only while that transfer's retry information is available, and rechecks captured inputs without repeating completed items. Retried cut moves preserve the existing clipboard ownership checks.

The most recent 12 results stay in memory until Clear finished or application exit. Clearing results does not cancel running jobs. Older reports can outlive their service, so Retry may no longer be available. This is progress and reporting, not a persistent recovery journal. Separate Luciole processes have separate job lists.

## Undo and redo a rename

Undo Rename (Ctrl+Z) reverses the most recent supported native rename. Redo Rename (Ctrl+Shift+Z) reapplies it. A fully completed batch rename is one action, shown as Undo Batch Rename or Redo Batch Rename in the main menu and folder background menu. The one-operation history is shared by tabs and windows within the same running Luciole process; it does not survive restart or cross independent processes. Text inputs retain their own text undo/redo.

Reversal checks the containing folder and every affected item's identity/revision, then renames without replacing anything. Changed, replaced or missing items are refused. An occupied original name is a conflict, never permission to overwrite; remove the conflict before retrying. Batch replay checks all affected items and destinations before changing names and handles name swaps and cycles. Items whose names did not change are excluded. Reading a file does not invalidate the record. Folder reversal checks the folder itself without traversing its contents.

Starting another mutating file job clears the previous history, including redo. Preparation-only dialogs and browsing do not clear it. A successful new single-item rename or fully completed batch supplies a replacement record when its history can be captured; a no-op rename supplies none. History is unavailable while file jobs run. A batch that fails or is cancelled partway through does not arm history. Cancellation or an intervening filesystem change during replay can also leave completed effects: inspect the File Operations report; incomplete replay retires history. Copy/move, creation, Trash, permission changes and overwritten-content recovery remain outside rename undo.

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
