# Folio parity

Folio (Omarchy's file manager) is the standard for behavior. This table
tracks each Folio feature in Flo: the core piece (flo-core) and the window
(src/ui/). "Done" means both exist; screenshots of the window are in
`docs/screenshots/`.

| Folio feature | Flo status |
|---|---|
| Listing off the UI thread, folders first, natural sort | Done (`Lister`) |
| Sort by name, kind, newest, oldest, size (saved name `modified`) | Done (`SortMode`) |
| Hidden files toggle | Done (`ListOptions::show_hidden`, setting `view.hidden`) |
| Folder item counts, counted lazily (visible first) | Done (`Lister::count_items`) |
| Live reload, debounced 250 ms | Done (`Watcher`) |
| Remembered recent listings for instant back/forward | Done (`ListingCache`, shared by every window): a folder's listing (with item counts and peeks) is moved into the cache when a pane leaves it or starts a search, and moved back out when one returns, so it is never held twice; it shows in the same frame and is re-read, kept as is when unchanged (`same_listing`). Bounded to 24 listings and 12,000 entries (Folio: 64 listings, no entry bound); `FLO_TRACE_LISTINGS=1` logs each recall. The columns view still lists its parent columns afresh |
| 50,000 entries in under a second | Done: 110–145 ms for 50k entries (test) |
| Progressive listing of slow (SFTP) folders | Done (snapshots after 150 ms) |
| Shared freedesktop thumbnail cache, failure markers | Done (`fail/flo/` instead of `fail/folio`) |
| Thumbnailers run outside the UI process with limits | Done: `flo-thumbnailer` workers (Folio still runs in-process for images) |
| Validate `Thumb::URI` on a hit | Done (also `Thumb::MTime`, `Thumb::Size`, chunk CRCs) |
| Folder peek (two pictures of a folder) | Done: `summarize_folder` picks the first two pictures (natural order, by extension, none on network filesystems) with the item count; the grid draws them on the folder card from the shared cache's 256 px bucket (a few KB each in the bounded image LRU), and the preview shows them 220 x 165 as Folio's folder view does. Folio shows the peek only in its preview; Flo also puts it on the tiles (`docs/screenshots/folder-peek.png`) |
| Copy, cut, paste interoperable with GNOME/KDE | Done (`clipboard`, `FileOps::paste`); the window reads and writes the clipboard |
| Pastes never overwrite (`name (copy).ext`) | Done (`free_name`) |
| Conflict prompt (replace, keep both, skip, apply to all) | Done (`ConflictPolicy::Ask`); Replace trashes the occupant |
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
| Drives: removable and extra internal disks, not the system's own | Done (mount table + GIO volume monitor), mount/unmount/eject; a drive is its own breadcrumb root. Verified in Xvfb with a mountinfo fixture (`FLO_TEST_MOUNTINFO`): listed with an eject button, opened, ejected while open (toast, pane goes home) |
| Dropbox sync status (`dropbox-cli`) | Done (`Network::refresh_dropbox`) |
| Tailnet machines from `tailscale status --json` | Done (`Network::refresh_tailnet`); verified in Xvfb with a fake `tailscale` (`docs/screenshots/drives-tailnet.png`) |
| SFTP over GIO landing in the matching home folder | Done (`Mounter`) |
| Host keys auto-accepted for tailnet peers only, password asked in the window | Done (questions for other hosts go to the UI too; a retry says the password didn't work). Verified in Xvfb with `FLO_TEST_FAKE_GVFS`, which drives the real `GMountOperation`: a tailnet machine asks only the password, another host asks GVfs's host-key question first, a wrong password asks again, Cancel shows no error (`docs/screenshots/sftp-prompt.png`) |
| `/` search: breadth first, same drive, no symlinked folders | Done (`Searcher`), streamed |
| Search with plocate | Done (Folio plan phase 2; Folio's code still walks only): when `plocate` is installed and its database answers, its matches below the folder come first (one scoped, case-insensitive `--regex` query, checked against the disk: gone, hidden, other filesystem or under a symlinked folder are left out; nearest first), then the live walk adds what the database doesn't know yet, without repeats. No plocate or no database: the walk alone, as before. Tested against the real `plocate` with a private `updatedb -o` database |
| Views remembered (grid/list/columns), details pane, window size | Done (`Settings`, `~/.config/flo/flo.conf`) |
| `org.freedesktop.FileManager1` | Done (`DBusService`); the window handles ShowFolders/ShowItems. Verified in Xvfb on a private dbus-daemon session bus (`FLO_SESSION_BUS=1`): Flo owns the name, `ShowItems` opens the folder with the item selected, `ShowFolders` opens a window, and a second `flo PATH` is forwarded to the running one |
| `folio <file>` opens its folder with the file selected | Done in `main.cpp` (forwarded to a running Flo) |
| New windows in the running process (Ctrl+N) | Done (single-instance name) |
| CLI `list|info|thumbnail|trash|restore --json` | Done (`flo list|info|thumbnail|trash|restore`, plus `trash --list`) |
| Open With / default apps | Done: "Open With" lists the applications GIO knows for the type, the default first (as Folio, only when there is more than one). Flo adds a "Set Default" submenu: picking an application there, and only then, runs `xdg-mime default APP.desktop TYPE` (GIO's own call without xdg-utils). Verified in Xvfb with private applications (`docs/screenshots/open-with.png`): the private `mimeapps.list` changes and the menu marks the new default |
| Share via Dropbox link, LocalSend, Taildrop, terminal here | Done, as Folio's Share menu: "Dropbox Link" for items in `~/Dropbox` while Dropbox runs (`dropbox-cli sharelink`, link copied), "LocalSend…" when `localsend` is installed (`localsend --headless send`), and a "Taildrop" submenu of the online tailnet machines (`tailscale file cp … HOST:`); no Share at all when none applies. Terminal here was already done. Verified in Xvfb with fake tools on a private PATH (`docs/screenshots/share.png`) |
| Preview process (`folio preview`) | Done: `flo preview PATH`, its own process |
| Taildrive shares, MTP | Not yet |
| Theme, translations, keyboard, `?` overlay | Done (live theme, Folio's keys, `?`); strings go through `fo::tr`, no translation catalogs yet |
