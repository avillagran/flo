# Folio parity

Folio (Omarchy's file manager) is the standard for behavior. This table
tracks the non-UI part: what flo-core provides for each Folio feature. "UI"
means the core piece exists and the window still has to use it.

| Folio feature | Flo status |
|---|---|
| Listing off the UI thread, folders first, natural sort | Core done (`Lister`) — UI |
| Sort by name, kind, newest, oldest, size (saved name `modified`) | Core done (`SortMode`) — UI |
| Hidden files toggle | Core done (`ListOptions::show_hidden`, setting `view.hidden`) — UI |
| Folder item counts, counted lazily (visible first) | Core done (`Lister::count_items`) — UI |
| Live reload, debounced 250 ms | Core done (`Watcher`) — UI |
| Remembered recent listings for instant back/forward | Not yet (the UI can keep `ListingPtr`s) |
| 50,000 entries in under a second | Done: 110–145 ms for 50k entries (test) |
| Progressive listing of slow (SFTP) folders | Core done (snapshots after 150 ms) — UI |
| Shared freedesktop thumbnail cache, failure markers | Done (`fail/flo/` instead of `fail/folio`) |
| Thumbnailers run outside the UI process with limits | Done: `flo-thumbnailer` workers (Folio still runs in-process for images) |
| Validate `Thumb::URI` on a hit | Done (also `Thumb::MTime`, `Thumb::Size`, chunk CRCs) |
| Folder peek (two pictures in folder tiles) | Not yet |
| Copy, cut, paste interoperable with GNOME/KDE | Core done (`clipboard`, `FileOps::paste`) — UI clipboard I/O |
| Pastes never overwrite (`name (copy).ext`) | Done (`free_name`) |
| Conflict prompt (replace, keep both, skip, apply to all) | Core done (`ConflictPolicy::Ask`); Replace trashes the occupant |
| Drop: move on the same drive, copy across | Done (`FileOps::drop`) |
| Duplicate, rename, new folder ("New Folder", "New Folder 2") | Done |
| Compress to zip (bsdtar, hidden temp folder, "Archive.zip") | Done |
| Extract | Done (single top-level item lands as itself, else a folder) |
| Trash with each drive's own `.Trash`, Restore, Delete Permanently, Empty Trash, origin and date | Done (own implementation of the spec, with receipts for undo) |
| Undo/redo for rename, move, copy, trash, new folder | Done, plus compress, extract, duplicate, replace; identity checks as in the Rust version |
| Cross-drive move | Copies, then trashes the originals (Folio unlinks them) |
| Operation queue with progress and cancel | Done (`FileOps`) |
| Sidebar Home: every visible home folder, XDG glyphs, Dropbox, `omarchy` → Omarchy | Done (`places::home_places`) |
| Pinned = GTK bookmarks, remote ones kept | Done; remote bookmarks are also listed |
| Recents from recently-used.xbel | Done (read; Flo also adds files it opens) |
| Drives: removable and extra internal disks, not the system's own | Done (mount table + GIO volume monitor), mount/unmount/eject |
| Dropbox sync status (`dropbox-cli`) | Done (`Network::refresh_dropbox`) |
| Tailnet machines from `tailscale status --json` | Done (`Network::refresh_tailnet`) |
| SFTP over GIO landing in the matching home folder | Done (`Mounter`) |
| Host keys auto-accepted for tailnet peers only, password asked in the window | Done (questions for other hosts go to the UI too) |
| `/` search: breadth first, same drive, no symlinked folders | Done (`Searcher`), streamed |
| Search with plocate | Not yet (Folio plan phase 2) |
| Views remembered (grid/list/columns), details pane, window size | Core done (`Settings`, `~/.config/flo/flo.conf`) — UI |
| `org.freedesktop.FileManager1` | Done (`DBusService`) — UI handlers |
| `folio <file>` opens its folder with the file selected | Done in `main.cpp` (forwarded to a running Flo) — GUI |
| New windows in the running process (Ctrl+N) | Core done (single-instance name) — UI |
| CLI `list|info|thumbnail|trash|restore --json` | Done (`flo list|info|thumbnail|trash|restore`, plus `trash --list`) |
| Open With / default apps | Not yet in core (GIO `g_app_info_*`, small) |
| Share via Dropbox link, LocalSend, Taildrop, terminal here | Not yet (simple child processes) |
| Preview process (`folio preview`) | Not in scope of the core |
| Taildrive shares, MTP | Not yet |
| Theme, translations, keyboard, `?` overlay | UI |
