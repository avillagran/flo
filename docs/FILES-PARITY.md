# Files parity

Flo is the C++ successor of **Files** (`avillagran/files-rust`, the Rust
app it replaces) and carries Files' identity: its look, layout, keys,
wording and feature set. This table tracks Files' features in Flo.

Omarchy's [Folio](https://github.com/omacom/folio) is a reference only for
desktop standards and plumbing (see the last section); where Folio and
Files differ in look, keys or wording, Files wins.

Screenshots: `docs/screenshots/files-vs-flo/` puts Files (left) and Flo
(right) side by side in the same states, taken by `tools/side-by-side.sh`
in two private Xvfb sessions with the same fixtures and palette;
`tools/flo-shots.sh` retakes Flo's own states.

Status words: **Done** (in Flo, checked in Xvfb), **Partial** (the
everyday part is there; what is missing is named), **Next** (planned for
the next phase), **Out** (left out of this phase on purpose).

## Look and layout

| Files | Flo |
|---|---|
| Omarchy palette from `colors.toml`, Tokyo Night outside Omarchy; Files' roles (cards at 8 % foreground, dim text, selection for marks) | Done (toolkit `fo::Theme::card/dim/muted/selection`) |
| Omarchy's Nerd Font for text and glyph icons (monospace if it is a Nerd Font, else JetBrainsMono/FiraCode Nerd Font) | Done (toolkit `text::ui_family`, `fo::glyph`; Lucide when no Nerd Font) |
| Header: back/forward/up, path chips (current on the accent, leading "…" chip), compact icon buttons on the right (Search, Edit path, Groups, List, Grid, Space, Columns, Inspector, Actions, Preferences at the far right), label revealed on hover, shortcut after a two-second rest | Done (`fo::ToolBar`, `fo::Breadcrumbs`). Under 640 px the views fold into one Views menu, as Files does |
| Free header area moves the window (a hand over it; the 24 px corner grips resize) | Done for moving (`fo::Window::move_area` / `begin_move`, after Files' `window_chrome.rs` and its native bridge): the header outside back/forward/up, the address card, the OK/× buttons and each toolbar button moves the window on a left press, never while a dialog or menu is open or in fullscreen. Wayland: FLTK 1.4's own public path (a reposition while only the left button is down sends `xdg_toplevel.move` with the press serial), so no private FLTK headers are needed; X11: EWMH `_NET_WM_MOVERESIZE`, or the window follows the drag without a window manager. The corner grips are not ported: Files reaches libdecor through FLTK's private Wayland driver, which Flo does not compile against; the compositor's own resize (Super+drag on Hyprland) covers it |
| Tab strip: current tab on a card with an accent line, ×, "+" on the right, middle click closes; tabs dragged to a new place (after 5 px an accent line shows the drop point, the release moves it, leaving the strip cancels); tab menu Close tab, Close other tabs, Close tabs to the right, Move left, Move right | Done (`fo::TabStrip::on_reorder`); Flo's New tab and Reopen closed tab follow in the tab menu |
| 52 px rail expanding to 170 px on hover (350 ms) or when pinned; Files' sections (`places_sidebar.rs`): Favorites (opens the favorites hub), Recents, Starred; Places: Home and the XDG folders that exist (`places.rs` `xdg_places`: Downloads, Documents, Pictures, Music, Videos, Desktop from `user-dirs.dirs`, falling back to their English names, skipped when missing or home itself); Bookmarks; Devices; Network; the trash at the bottom with the pin control | Done (`fo::Sidebar::rail`, `places::xdg_places`). Place glyphs follow Files' `place_glyph(index)`: by the place's position in the list (home, download, document, picture, music, film, then the database glyph), so a missing XDG folder shifts the glyphs of the ones after it, exactly as in Files. Flo's Bookmarks are the GTK bookmarks (Files keeps its own file); remote GTK bookmarks and tailnet machines are listed under Network too. **Reordering** (Files' `sidebar_order.rs`, navtree `Reorder::Siblings`): places, devices and bookmarks drag within their section (after 5 px a 2 px accent line shows the drop point; a drag never navigates, a click opens on release) or move with "Move up" / "Move down" in the row menu; places and devices keep Files' `rail_order` keys in `[rail] order` (`core/rail_order.h`), bookmarks move their own line in the GTK bookmarks file (`places::move_bookmark`; remote and other lines stay put). **Network** ("Red", always shown): the connected GVfs locations read from the FUSE bridge (`scan_gvfs`, Files' names such as "fotos on nas", Disconnect on the row's eject button and in its menu, open tabs leave the location first), then Files' "Connect…" row (a spinner while a connection or disconnection runs; its menu says "Connect to server…"). "Connect…" opens the location field with `smb://`; Enter checks the address as Files' `network::parse` does (scheme smb/sftp/ssh/ftp/ftps/dav/davs/nfs/afp, a server, no spaces, ≤ 2048 bytes; a bad one stays in the field with Files' reason) and mounts it through GVfs with Flo's sign-in and host-key dialogs (an inline `user:password@` answers the first password request once and never reaches the URI). Files' second detail line on network rows (protocol · user) has no room in the toolkit's one-line rows |
| Inspector (240 px): preview tile, name, kind, total size with "Recalculate · Ctrl+Space", "Rename · F2", Open / Preview, copy path / F5 / F6 | Done (`DetailsPane`); Flo adds picture dimensions and a trashed item's origin |
| Status bar: marked and listed counts, a tip, filesystem and free space, live PSS | Done (`fo::StatusBar`). Files' nine tips (`status_tips.rs`) take turns every 15 s and name their keys in the active shortcut profile (a command without a key there points to the command palette); Files scrolls a long tip, Flo shows it elided |
| Groups view: folder cards under "FOLDERS · n" with a new-folder button, files as rows under TODAY / THIS WEEK / EARLIER / UNDATED | Done (`Collection::Layout::Bands`) |
| List view: 38 px rows, thumbnails or extension badges, size / date / type columns, sortable header | Done; Files' sort directions (`model.rs` `Action::Sort`): clicking the active column flips it (name A→Z / Z→A, size biggest / smallest first, date newest / oldest, type A→Z / Z→A with names A→Z inside a type), another column starts A→Z for name and type and biggest / newest first for size and date; folders stay first |
| Grid view: 156 x 132 cells with thumbnails; zoom 65–180 % (Ctrl++ / Ctrl+= / Ctrl+- / Ctrl+0, Ctrl+wheel, the − / + buttons) and the 36 px "Grid · n · thumbnails" header with the percentage | Done (cells, icons and the header follow Files' `grid_view.rs`; outside the Grid the keys say "Zoom only exists in the Grid view (Ctrl+5)") |
| Columns view: Finder-style columns, ← → change column, Shift+wheel scrolls, item counts | Done |
| Space view: treemap by apparent size, tooltips, arrows move spatially, Enter zooms into a folder, Backspace back; the 108 px toolbar (title with the visible total, Show/Hide hidden, Back, Recalculate, Cancel while measuring, the apparent total, the watch notes, Marks), F5 recalculates; the "No area/label" list of what has no area or is too small for a label (three rows, wheel and ↑↓, Down from the map enters it) | Done (`SpaceView`, after Files' `space_map_view.rs`): Files' weighted binary treemap; every block in Files' stable colour of its name (blue, cyan, magenta, green, yellow; `ui/tile_colors.h`) with a label colour of contrast ≥ 4.5, name, type icon (or a glyph before the name) and size ("· N files" on wide folders); marks and hover as the light band, keyboard focus as the accent ring gliding between blocks (80 ms); Files' tooltip placement. Motion from `space_map_zoom.rs`: opening a folder zooms its block to the whole map and going up shrinks the map into its block (220 ms, OutCubic, the last 8 layouts kept), children grow in (140 ms) when the sizes arrive after the zoom, the folder's outline waits at the map edge while they are measured; nothing runs with animations off and no frame is asked for once the motion ends. While the sizes are measured, Files' Omi (`omi.rs`, `ui/omi.h`: the Omarchy logo as 13 rectangles with eyes that blink, roll every fourth turn and lean toward the pointer) sits 60 px under the left-aligned "Calculating sizes…", 72 px wide; it asks for 33 ms frames only during a turn or while the eyes glide, waits for the next turn otherwise, and its timer stops when the map is ready (no idle wake-ups). The Back button says "Go back" (es: "Volver", as Files); the tooltip reads "Folder · 304 kB · 2 files · 1 folder" with singular and plural forms |
| Dual pane (F9): two list panes with their own header, footer and filter; Tab switches; F5/F6 target the other pane | Done; the footer reads Files' "12 visible / 40 loaded · 2 marked" with singular forms |
| Menus: Actions (F10) and Preferences (Ctrl+,) as grouped submenus; context menu in Files' order and wording | Done (Flo's extras stay in them: Share, Pin, Open with → Set default) |
| Commands palette (Ctrl+Shift+P, or Ctrl+L then `>`), places catalog (`:`) | Done (`CommandPalette`), with Files' footer buttons: "Run" (the highlighted entry, muted when it can't run) and "Use current context" (rebuilds the entries and their enabled state from the window as it is now) |
| Shortcut help (F1) | Done |
| Empty states ("Empty", "No matches", "Select an item") | Done |

## Keys (Files' default profile)

| Keys | Flo |
|---|---|
| Type to filter, Esc clears, Ctrl+F edits the filter; `*.pdf`, `ext:pdf,md`, `type:dir`, `!draft` | Done (`core/name_filter.h`) |
| Ctrl+L path / omnibar (`>command`, `:place`), ✓ and × buttons, Tab completes | Done. Files' editing (`path_edit.rs`, `line_edit.rs`): Ctrl+Left/Right (Shift extends) jump over whole components between `/`, Ctrl+Backspace/Delete remove one, a double click selects one, Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y undo and redo (FLTK merges adjacent deletions into one undo step); Tab completes the last component (`core/path_completion.h`: one match is taken whole, several extend to their common prefix and list in the status bar as "Tab · a/ [b/] c/", Tab / Shift+Tab / Up / Down walk them, Enter accepts, Esc closes; folders win, hidden names only after a typed ".", case-insensitive when nothing else matches, one bounded read, network locations not read). Files has no text menu of its own in the field (a right click opens the general context menu there), so none is added |
| Ctrl+Shift+F recursive search | Done (Flo's plocate-then-walk search); search facets: Out |
| Ctrl+1..5 Groups, List, Columns, Space, Grid | Done |
| Ctrl+A, Ctrl+I invert, Ctrl+Shift+A mark filtered, Ctrl+Shift+D clear, Insert marks and advances, Ctrl+Insert marks without moving | Done; Ctrl+arrows and Ctrl+Home/End move the focus without touching the marks and Ctrl+Shift adds the range to them (Files' `keyboard_nav.rs`, toolkit `Collection`) |
| Enter, Backspace / Alt+Up, Alt+Left/Right, Alt+Home, Alt+Down (ancestors menu) | Done |
| Ctrl+Enter / Shift+Enter open a folder in a new / background tab | Done |
| Ctrl+T, Ctrl+W, Ctrl+Shift+T, Ctrl+Tab / Ctrl+PgDn, Ctrl+Shift+Tab / Ctrl+PgUp (up to 8 tabs), Ctrl+Shift+PgUp / PgDn move the active tab | Done; the last session comes back at start (see Behavior) |
| F2 rename | Done (a single rename; the advanced renamer: Out) |
| F5 copy / F6 move (destination reviewed first) | Done (a destination prompt, then Flo's queue with conflicts and undo). Files' copy card at the bottom of the list: what is copied, bytes, entries and the operations waiting, the current item, Pause / Resume (the worker stops between files and between 4 MiB chunks), Cancel job and Clear pending (cancels the queued operations before they start). Files' rescue of interrupted jobs (a journal of each copy replayed at the next start) and its start-paused review form: Out (a journaled copy engine of its own; Flo's queue keeps conflicts, undo and cancellation) |
| F7 / Ctrl+Shift+N new folder, Ctrl+N new file | Done: both run on the operations worker and are recorded for undo (`FileOps::new_file`: created exclusively, never replacing a name or writing through a link; the record comes from the descriptor that made it). Ctrl+Z sends the new file to the trash only while it is still exactly what was made (same device and inode, same size and modification time, no change since); Ctrl+Shift+Z / Ctrl+Y put it back |
| Space / F3 preview | Done (Flo's preview process) |
| Delete / F8 trash, Shift+Delete | Done (permanent deletion only inside the trash, after a prompt) |
| Ctrl+C / X / V, Ctrl+Shift+V, Ctrl+Alt+C copy path | Done |
| Ctrl+Space size of the focused folder, Ctrl+Shift+Space listed folders | Done (`FolderSizes`: no links followed, one filesystem, 256 results kept). The size manager (Ctrl+Alt+Space): Out |
| Ctrl+H hidden, Ctrl+R reload, Menu / Shift+F10 context menu, Ctrl+Alt+I inspector, F9 dual, F10 Actions, Ctrl+, Preferences, Ctrl+D pin, Ctrl+Alt+S rail, Ctrl+. terminal, Ctrl+Alt+R Recents, Ctrl+Shift+O open with, F1 help | Done (Ctrl+Alt+S focuses the rail) |
| Alt+Enter properties | Done (`PropertiesDialog`, `core/properties.h`, after Files' `properties_dialog.rs`): up to 32 marked objects (Alt+Left/Right, Previous / Next); kind, apparent and allocated size, hard links, owner and group, symbolic and octal mode, link target (never followed), MIME type with the default application, volume, device and inode, modified / accessed / created / inode-change dates, the requested and verified paths, and the contents of a folder or the selection (counted off the UI thread, no links followed, one filesystem). The r/w/x bits (Tab, Space, clicks, "Allow executing as a program") apply with Enter or Apply permissions, by descriptor after checking device, inode, owner and mode; links, special objects, other owners and setuid/setgid/sticky stay read-only. Also in Actions and the context menu. The contents and selection counts use singular and plural forms ("1 file, 2 folders", "Selection of 3 objects") |
| Ctrl+Alt+D favorites and places | Done (`FavoritesHub`, after Files' `bookmarks_ui.rs`): favorites with their labels, the XDG places and the tab's Back / Forward history; typing filters by label, Enter opens, F2 edits the label (stored as the GTK bookmark label; the folder is never renamed; 1–96 bytes, one line), Delete removes a favorite (the folder stays), Ctrl+D adds the current folder, F5 reloads. Files' publish/ACK diagnostics (F1, F8) have no counterpart: Flo writes the bookmarks file directly |
| Ctrl+E star, Ctrl+Alt+E starred | Done (`core/starred.h`, Files' format in `$XDG_DATA_HOME/flo/starred`, up to 10,000 items): Ctrl+E stars the marked items (or the focused one) or unstars them when all are starred; a yellow star marks them in the List and Groups rows; Starred (the rail, Ctrl+Alt+E) lists them newest first with their folder, keeps missing ones as "unavailable", and its menu offers only Open, Open containing folder, Remove from Starred (Delete; the file is not touched) and Star. In Recents, Delete and "Remove from Recents (the file is not deleted)" take the entries off GTK's list (`recents::remove`, through GLib's bookmark file; other apps' entries stay and a file GLib can't read back whole is left untouched) |
| Ctrl+Alt+B MC operation bar | Done (`OperationBar`, after Files' `operation_bar.rs`): a 36 px row above the status bar with F1 Help, F2 Rename, F3 Preview, F5 Copy (Recalculate in the Space view), F6 Move, F7 Folder, F8 Trash, F10 Actions; muted buttons say why when clicked; kept in the settings (`operation-bar`) and in Preferences → View |
| Nautilus shortcut profile | Done (`core/shortcuts.h`, Files' `shortcuts.rs` table): Preferences → Session and help → "Shortcuts: Files" / "Shortcuts: Nautilus" (also in the command palette), applied live to every window and kept in the settings (`[keys] profile`). Nautilus moves F5 to Reload, F6 to switching pane (or rail and list), F9 to keeping the rail open, F3 to the dual pane, Ctrl+1..5 to List, Grid, Groups, Columns, Space, Ctrl+I to Properties, Ctrl+Shift+I to invert, Ctrl+N to a new window, Ctrl+Space to marking, Alt+Down to opening, Ctrl+F to the recursive search and Ctrl+Shift+F to the filter; copy, move, new file, the size of the focused folder and the ancestors menu have no keys there and stay in the menus. The window keys, the help (F1, which names the profile), menu accelerators, the toolbar, the inspector and the MC bar print the active profile's chords |

## Behavior

| Files | Flo |
|---|---|
| Automatic folder sizes for visible local folders, one helper, no links followed | Done (a worker thread over `core/tree_size.h`; folders of remote and trash locations are not measured) |
| The first entry is focused on arrival; the selection survives reloads | Done |
| Hidden files off by default, persisted | Done |
| Preferences: window normal / fullscreen, show thumbnails, automatic sizes, keep the rail open | Done, with Files' "Interface borders" (Omarchy: Hyprland's rounding, Square, Rounded) and "Theme" (System · live, Tokyo Night · local view) in Preferences → Appearance, kept as `[view] corners` and `theme` and applied over the live theme (toolkit `fo::theme_overrides`; the Omarchy theme keeps being followed underneath) |
| Sessions and saved per-folder views | Done: "Save / Forget this folder's view" (Preferences → Folder, `core/folder_views.h`, up to 256 folders) keeps a folder's view, sort and zoom and applies them whenever it opens; folders without one keep what the pane shows. `flo` without a folder restores the last session (`core/session.h`: tabs with history, view, sort, zoom, filter, marks and focus, the closed tabs, the active tab); it is saved when that window closes and with "Save session"; `flo PATH` and `flo --no-session` leave it alone |
| Directory sync and compare | Out |
| Size manager | Out |
| Advanced renamer (patterns, sequences, regex) | Out |
| Search facets (size and date filters) | Out |
| Translations | Done for all 50 catalogs (`tools/i18n-check.py`: 50/50 complete, placeholders and CLDR plural forms checked). Files itself ships only Spanish: es-MX uses Files' own wording wherever a string matches (tab menu, completion notices, "Atajos: Files / Nautilus", the status tips); the other 48 languages are machine translations that keep each catalog's terms, still waiting for a native review |

## Desktop standards (Folio as the technical reference)

Kept from the work that followed Folio's plumbing; none of it changes the
look or the keys above.

| Standard | Flo |
|---|---|
| Shared freedesktop thumbnail cache (128/256/512/1024 buckets, `fail/flo/`), sandboxed `flo-thumbnailer` workers | Done |
| Folder peek (two pictures of a folder) | Done, on Grid folder cells and in the preview |
| freedesktop trash per drive, restore, empty, undo through the trash | Done |
| GTK bookmarks (pins), Recents (`recently-used.xbel`) | Done |
| `org.freedesktop.FileManager1`, single instance, `flo PATH` | Done |
| Drives with mount / unmount / eject (GIO), mounted MTP devices | Done (MTP compiled only) |
| Tailnet machines over SFTP (GVfs), the SFTP password prompt in the window | Done |
| Share: Dropbox link, LocalSend, Taildrop; Open with and Set default | Done |
| Live Omarchy language, plural rules | Done |
| Remembered listings for instant back/forward | Done |
| Search with plocate, then a live walk | Done |
| CLI `flo list|info|thumbnail|trash|restore [--json]` | Done |

## Not ported, and why

| Files | Why not in Flo |
|---|---|
| The 24 px corner grips that resize the window | Files reaches libdecor through FLTK's private Wayland driver; Flo builds only against FLTK's public headers. The compositor's own resize (Super+drag on Hyprland) covers it |
| Rescue of interrupted copies and the start-paused copy review | Needs a journaled copy engine of its own (Files' `copy_review.rs`, `archive_recovery.rs`); Flo's queue keeps conflicts, undo and cancellation instead |
| Directory sync and compare, the size manager, the advanced renamer, search facets | Separate tools in Files, out of this phase (see the rows above) |
| The second line of network rows (protocol · user) | The toolkit's rail rows are one line; the location's name keeps the room |
| Omi in the status bar while Files is busy (`view.rs`) | Not ported yet: Flo's status bar shows the work in its text; Omi appears in the Space map's calculating state |
