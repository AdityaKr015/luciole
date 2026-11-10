# File operations

## Copy, move and create

Use Ctrl+C or Ctrl+X for the selection and Ctrl+V in the destination folder. Paste is also available from context menus, including List view. Clipboard transfers and drag/drop work between Files windows; the same transport supports other file managers.

Drop files on a folder or the destination pane. Ctrl requests Copy, Shift requests Move and Alt offers an action chooser. Symbolic-link drops are supported for native locations. Dragging a folder into Places pins it instead of transferring its contents.

When names collide, the conflict dialog offers Replace, Skip and a numbered new name. Folder copies can merge into existing folders. Apply-to-all choices are scoped to the current job. Failed-item retries are explicit. Cancellation stops remaining work; it does not undo already completed changes. Transfer undo/redo and interrupted-job recovery are not implemented yet.

The background menu provides New Folder (Ctrl+Shift+N) and New Document. New Document offers an empty file and files from the configured XDG Templates folder. Create Link on a single native item creates a symbolic or hard link where supported.

F2 renames one item. Multiple selected native items from the same containing folder open batch rename. Preview the proposed names before applying them.

## Undo and redo a rename

Undo Rename (Ctrl+Z) reverses the most recent supported single-item native rename. Redo Rename (Ctrl+Shift+Z) reapplies it. Both actions are in the main menu and the folder background menu. The one-operation history is shared by tabs and windows within the same running Files process; it does not survive restart or cross independent processes. Text inputs retain their own text undo/redo.

Reversal checks the containing folder and the renamed item's identity/revision, then renames without replacing anything. Changed, replaced or missing items are refused. An occupied original name is a conflict, never permission to overwrite; remove the conflict before retrying. Reading a file does not invalidate the record. Folder reversal checks the folder itself without traversing its contents.

Starting another mutating file job clears the previous history, including redo. Preparation-only dialogs and browsing do not clear it. A successful new single-item rename supplies a replacement record; a no-op rename supplies none. History is unavailable while file jobs run. Batch rename, copy/move, creation, Trash, permission changes and overwritten-content recovery remain outside this first undo increment.

## Trash

Delete moves native selections to Trash. A failed trash operation does not silently become permanent deletion. Trash requires the system's GIO/GVfs provider.

In Trash, Restore returns selected items to their recorded original locations and asks about conflicts. Delete Permanently removes selected trashed items after confirmation. Empty Trash is available from the Trash sidebar row's context menu; its confirmation includes the measured contents and size. Permanent deletion is irreversible.

## Archives

Compress creates ZIP, TAR and 7z archives for native selections. Supported compressed TAR variants depend on the installed libarchive build and include gzip, bzip2, XZ and Zstandard. Browse Archive opens a read-only archive page; Extract Here and Extract To publish extracted contents through the normal transfer/conflict flow. Archive browsing can extract selected members or open an individual file through its associated application.

ZIP/TAR/7z/RAR input depends on available archive capabilities. Encrypted 7z input additionally uses a system 7-Zip executable. Passwords are requested when needed and are not stored in settings. Encrypted RAR is not supported. Numbered byte-split ZIP sets use `name.zip.001`, `.002` and so on; select `.001`, with all contiguous parts beside it. Spanned `.z01` sets and split-archive creation are not supported.

Opening and extracting archives validate members and enforce resource limits. An archive rejected as unsafe or unsupported is reported; Files does not silently disable these checks. Native archives are the current scope.

## Properties and permissions

Properties (Alt+Return) shows available file/media facts. Images can include pixel dimensions, videos can include codecs and duration, and PDFs can include page information. Installed providers determine which facts are available.

For folders, Properties starts a separate cancellable recursive count/size calculation. Ordinary selection and the side preview do not traverse folders to calculate totals. Recursive totals are currently unavailable for non-native locations and Trash.

Native permission and ownership editing is available in Properties. Multiple selections support bulk changes; recursive changes are explicit operations. Advanced permissions opens the POSIX ACL editor on supported native filesystems. Merely selecting a folder does not apply anything recursively. Files reports unsupported or denied changes rather than claiming they succeeded.

## Network locations

Mounted, authenticated SFTP, SMB and WebDAV locations support the verified copy/move paths through installed GIO/GVfs backends. Some providers cannot preserve native symlinks, raw filename bytes or every metadata field. Read-only targets, conflicts, disconnects and partial moves remain explicit outcomes. Files does not provide its own server connection or credentials dialog yet.

See [runtime providers](../../PACKAGING.md#gvfs-providers-across-distributions) for distribution package requirements and [the remaining plan](../../PLAN.md) for unfinished device/network integration.
