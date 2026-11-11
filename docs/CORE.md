# flo-core

`flo-core` is Flo's non-UI core: a static library in `src/core/`, namespace
`flo`, written in C++17 against POSIX and GLib/GIO only. It never includes
FLTK or the toolkit, so it builds, tests and runs without a display. The
`flo` binary links it for the CLI today and for the GUI later.

## Threading model

Nothing in the core blocks the UI thread. Blocking work runs on worker
threads (`Executor`, one serial thread per service, started on demand and
gone after a few idle seconds), on the GLib thread (`GMainThread`, for GIO
mounts, the volume monitor and D-Bus) or in child processes (thumbnailing,
`bsdtar`, `tailscale`, `dropbox-cli`).

Every service reports through an `EventQueue`. Workers post closures; the UI
thread runs them with `dispatch()`. The queue owns an eventfd that is
readable while something is pending, so the FLTK UI integrates it once:

```cpp
flo::EventQueue queue;   // lives as long as the app
Fl::add_fd(queue.fd(), FL_READ, [](int, void *q) {
    static_cast<flo::EventQueue *>(q)->dispatch();
}, &queue);
```

All callbacks given to core services run inside `dispatch()`, on the UI
thread. Services hold a weak guard, so a callback never runs after its
service was destroyed, and results of overtaken requests (a folder left,
a search retyped, a thumbnail cancelled) are dropped before they reach the
UI. A CLI or test loops on `queue.wait(ms)` and `queue.dispatch()`.

## Headers

| Header | Main types | What for |
|---|---|---|
| `event_queue.h` | `EventQueue`, `CancelToken`, `AliveGuard`, `Executor` | Delivery to the UI thread, cancellation, serial workers |
| `entry.h` | `Entry`, `Kind`, `SortMode`, `sort_entries` | Folder entries (type by GIO content type, sniffed only when the name doesn't tell), Folio's kinds and five sort modes, folders first |
| `natural_sort.h` | `natural_compare`, `fold_case` | Case-insensitive natural order ("file2" < "file10") |
| `lister.h` | `Lister`, `Listing`, `list_folder`, `count_items` | Off-thread listing with snapshots for slow folders, lazy folder item counts |
| `watcher.h` | `Watcher` | inotify live reload, debounced (250 ms), folder or single file (recently-used.xbel) |
| `search.h` | `Searcher`, `search_locate`, `search_walk` | `/` search: plocate's matches first when it has a database, then a breadth-first walk for the rest; same filesystem, no symlinked folders, streamed, cancellable |
| `thumbnail_cache.h` | `ThumbnailCache`, `read_png_meta`, `thumbnail_matches` | The shared freedesktop cache read as untrusted input |
| `thumbnailers.h` | `ThumbnailerRegistry`, `expand_thumbnailer_exec` | Installed `.thumbnailer` entries |
| `thumbnail_service.h` | `ThumbnailService`, `ThumbnailResult` | Thumbnails for the UI via `flo-thumbnailer` worker processes |
| `trash.h` | `trash::put/list/restore/delete_permanently/empty_all` | freedesktop Trash, home and per-volume |
| `undo.h` | `UndoHistory`, `UndoEntry`, `verify_created` | Undo/redo with identity checks |
| `fileops.h` | `FileOps`, `OpResult`, `OpProgress`, `OpConflict` | The operation queue: copy, move, paste, drop, duplicate, rename, new folder, compress, extract, trash, restore, delete permanently, empty trash, undo, redo |
| `clipboard.h` | `clipboard::*` | `x-special/gnome-copied-files`, `text/uri-list`, KDE cut flag, plain text |
| `places.h` | `places::*`, `Place`, `Devices` | Home places, GTK bookmarks, Recents/Trash views, drives; mount/unmount/eject |
| `recents.h` | `recents::read/list/add` | GTK's recently-used.xbel |
| `network.h` | `Network`, `TailnetPeer` | Tailnet machines and Dropbox status |
| `mounter.h` | `Mounter`, `MountRequest` | GVfs mounts (SFTP to tailnet machines, remote bookmarks) |
| `dbus_service.h` | `DBusService`, `forward_open_to_running_flo` | `org.freedesktop.FileManager1` and single instance |
| `settings.h` | `Settings`, `Ini` | `~/.config/flo/flo.conf` |
| `gmain_thread.h` | `GMainThread` | The GLib main loop thread |
| `process.h` | `run_process`, `spawn_pipe_child` | Child processes with rlimits, timeouts, cancel, capped output |
| `fsutil.h`, `uri.h`, `xdg.h`, `json.h` | helpers | Paths, `free_name`, no-replace renames, atomic writes, mountinfo; GLib-compatible URIs; XDG dirs read live from the environment; a small JSON reader |

## How the UI uses it

- **Folder view.** One `Lister` per pane. `list(folder, options, cb)` gives
  `ListingPtr` snapshots (`complete == false`) and a final listing. Ask
  `count_items(visible folders first)` after it lands. Register the folder
  with `Watcher::watch_folder` and list again on change. Recents:
  `recents::list(recents::xbel_path(), ...)` on a worker, plus
  `Watcher::watch_file(xbel)`.
- **Thumbnails.** One `ThumbnailService` per process. Call `lookup()` for an
  immediate cache hit when painting, else `request(path, pixels, cb)` for
  visible tiles; `cancel(ticket)` when they scroll away. Load
  `result.thumbnail` (a small PNG) with `Fl_PNG_Image`.
- **File operations.** One `FileOps` per window. Show `on_progress`, ask on
  `on_conflict` (policy `Ask`) and answer with `resolve_conflict`, select
  `OpResult::created` in `OpResult::folder` on `on_done`, label the Undo and
  Redo menu items from `on_history`. `delete_permanently` and `empty_trash`
  need `confirmed = true` after a dialog.
- **Clipboard.** Build payloads with `clipboard::gnome_copied_files`,
  `uri_list`, `plain_text`; parse with `parse_any`; then
  `FileOps::paste(paths, folder, cut)`.
- **Sidebar.** `places::home_places`, `places::pinned(read_bookmarks())`,
  `places::remote_bookmarks`, `places::views()`, `Devices::watch()`,
  `Network::refresh_tailnet/refresh_dropbox` every 15 s while a window is
  visible (Folio's poll; nothing runs when no window is open).
- **Opening a tailnet machine.** `Mounter::mount(sftp_uri(host),
  network.is_tailnet_host(host), callbacks)`. A password prompt arrives as
  `MountRequest`; answer with `reply_password` (kept by GVfs for the session
  only) or `abort`. `on_mounted` gives the landing folder.
- **Open a file.** After launching it, call `recents::add(path, mime)` on a
  worker, like GTK apps do.
- **D-Bus.** `DBusService::start(true, on_ready)` at startup; handlers open
  windows or select items. `main()` already calls
  `forward_open_to_running_flo` before starting a GUI.

## Why a GLib thread and not a helper process

GVfs mounts, their questions, the volume monitor and GDBus need a running
GMainContext. FLTK has its own loop, and iterating GLib from FLTK timers
would add latency or busy polling. FLTK already loads GIO and D-Bus, so a
dedicated thread with its own context costs a thread stack and nothing in
libraries; a helper process would load GIO a second time (several MiB of
PSS) and need its own protocol. The thread starts only when a mount, the
device monitor or the D-Bus service is first used.

## Safety rules (from the Rust predecessor)

- Nothing is overwritten: renames use `renameat2(RENAME_NOREPLACE)` (link +
  unlink fallback), new files are written to a hidden partial file and
  published without replacing, folders are made with exclusive `mkdir`.
- Deleting means the trash. Trashing never copies across filesystems; an
  item with no trash on its own volume is refused and left alone.
- Moves across filesystems copy, then send each original to its own volume's
  trash (undoable) instead of unlinking it.
- Permanent deletion (`trash::delete_permanently`, `empty_all`) acts only on
  items directly inside a known trash's `files/`, needs an explicit
  confirmation flag, never follows symlinks and never leaves the item's
  filesystem. It is the only recursive removal in the code base; a test
  guards that.
- A failed or cancelled extraction's partial output goes to the trash. The
  only files the core unlinks directly are its own temporary files
  (partial copies, scratch thumbnailer output, a partial archive).
- Undo never deletes: see `undo.h`. Objects are recorded with their identity
  when created or moved and checked again right before acting; created
  objects must still be unchanged (files: size and mtime; folders: every
  entry's ctime before the operation's fence, within 20,000 entries); new
  folders must be empty. What fails a check is kept and listed.
- Child processes get a fixed argv (no shell), their own process group, a
  wall timeout that kills the group, rlimits and capped output.
- The thumbnail cache and thumbnailer output are untrusted input.

## Measurements (NODE, 2026-11-11, RelWithDebInfo)

| What | Result |
|---|---|
| `flo list` on 10,000 entries | 11.4 MiB peak RSS, 27 ms |
| `flo list --json` on 10,000 entries, while writing | 12.1 MiB RSS, 6.1 MiB PSS |
| `list_folder` on 50,000 entries (test) | 110–145 ms |
| `flo-thumbnailer` worker, idle after a request | 6.9 MiB RSS, 1.0 MiB PSS |

Most of the RSS is GLib/GIO and the shared MIME database, which FLTK loads
anyway; PSS is what Flo adds.

## Tests

`ctest --test-dir build` runs `flo-tests` through `tests/run-isolated.sh`,
which creates `build/test-runs/run-<time>-<random>/` (0700) with its own
`TMPDIR`, `HOME`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME`,
`XDG_STATE_HOME`, `XDG_RUNTIME_DIR` and an unreachable session bus, and keeps
it afterwards. The test binary refuses to run outside such a directory.
Fixtures come from `flo_test::private_dir()`; symlink fixtures point inside
their fixture; nothing deletes a tree.
