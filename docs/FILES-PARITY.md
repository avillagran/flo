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
| Free header area moves the window | Next (the compositor's own move works; FLTK has no portable interactive move) |
| Tab strip: current tab on a card with an accent line, ×, "+" on the right, middle click closes | Done (`fo::TabStrip`) |
| 52 px rail expanding to 170 px on hover (350 ms) or when pinned; pinned/recent first, Home, Downloads, Documents, Pictures, Music, Videos, the trash at the bottom with the pin control | Done (`fo::Sidebar::rail`); Flo also lists its other home folders, drives and tailnet machines there |
| Inspector (240 px): preview tile, name, kind, total size with "Recalculate · Ctrl+Space", "Rename · F2", Open / Preview, copy path / F5 / F6 | Done (`DetailsPane`); Flo adds picture dimensions and a trashed item's origin |
| Status bar: marked and listed counts, a tip, filesystem and free space, live PSS | Done (`fo::StatusBar`); the tip is static (Files scrolls its tips) |
| Groups view: folder cards under "FOLDERS · n" with a new-folder button, files as rows under TODAY / THIS WEEK / EARLIER / UNDATED | Done (`Collection::Layout::Bands`) |
| List view: 38 px rows, thumbnails or extension badges, size / date / type columns, sortable header | Done; sorting by name, size, date (newest/oldest) and type. Descending name order: Next |
| Grid view: 156 x 132 cells with thumbnails; zoom 65–180 % (Ctrl++ / Ctrl+= / Ctrl+- / Ctrl+0, Ctrl+wheel, the − / + buttons) and the 36 px "Grid · n · thumbnails" header with the percentage | Done (cells, icons and the header follow Files' `grid_view.rs`; outside the Grid the keys say "Zoom only exists in the Grid view (Ctrl+5)") |
| Columns view: Finder-style columns, ← → change column, Shift+wheel scrolls, item counts | Done |
| Space view: treemap by apparent size, tooltips, arrows move spatially, Enter zooms into a folder, Backspace back; the 108 px toolbar (title with the visible total, Show/Hide hidden, Back, Recalculate, Cancel while measuring, the apparent total, the watch notes, Marks), F5 recalculates; the "No area/label" list of what has no area or is too small for a label (three rows, wheel and ↑↓, Down from the map enters it) | Done (`SpaceView`, after Files' `space_map_view.rs`; Flo has no zoom animation between folders) |
| Dual pane (F9): two list panes with their own header, footer and filter; Tab switches; F5/F6 target the other pane | Done |
| Menus: Actions (F10) and Preferences (Ctrl+,) as grouped submenus; context menu in Files' order and wording | Done (Flo's extras stay in them: Share, Pin, Open with → Set default) |
| Commands palette (Ctrl+Shift+P, or Ctrl+L then `>`), places catalog (`:`) | Done (`CommandPalette`); Files' "Run" and "Use current context" buttons: Next |
| Shortcut help (F1) | Done |
| Empty states ("Empty", "No matches", "Select an item") | Done |

## Keys (Files' default profile)

| Keys | Flo |
|---|---|
| Type to filter, Esc clears, Ctrl+F edits the filter; `*.pdf`, `ext:pdf,md`, `type:dir`, `!draft` | Done (`core/name_filter.h`) |
| Ctrl+L path / omnibar (`>command`, `:place`), ✓ and × buttons, Tab completes | Done; Files' in-path component jumps and text undo: Next |
| Ctrl+Shift+F recursive search | Done (Flo's plocate-then-walk search); search facets: Out |
| Ctrl+1..5 Groups, List, Columns, Space, Grid | Done |
| Ctrl+A, Ctrl+I invert, Ctrl+Shift+A mark filtered, Ctrl+Shift+D clear, Insert marks and advances | Done; Ctrl+Insert (mark without moving) and Ctrl+arrows moving the focus without marks: Next |
| Enter, Backspace / Alt+Up, Alt+Left/Right, Alt+Home, Alt+Down (ancestors menu) | Done |
| Ctrl+Enter / Shift+Enter open a folder in a new / background tab | Done |
| Ctrl+T, Ctrl+W, Ctrl+Shift+T, Ctrl+Tab / Ctrl+PgDn, Ctrl+Shift+Tab / Ctrl+PgUp (up to 8 tabs) | Done; the last session comes back at start (see Behavior); moving tabs (Ctrl+Shift+PgUp/PgDn): Next |
| F2 rename | Done (a single rename; the advanced renamer: Out) |
| F5 copy / F6 move (destination reviewed first) | Done (a destination prompt, then Flo's queue with conflicts and undo); Files' copy queue with pause and rescue: Next |
| F7 / Ctrl+Shift+N new folder, Ctrl+N new file | Done (a new file is created exclusively, never overwriting; it has no undo yet) |
| Space / F3 preview | Done (Flo's preview process) |
| Delete / F8 trash, Shift+Delete | Done (permanent deletion only inside the trash, after a prompt) |
| Ctrl+C / X / V, Ctrl+Shift+V, Ctrl+Alt+C copy path | Done |
| Ctrl+Space size of the focused folder, Ctrl+Shift+Space listed folders | Done (`FolderSizes`: no links followed, one filesystem, 256 results kept). The size manager (Ctrl+Alt+Space): Out |
| Ctrl+H hidden, Ctrl+R reload, Menu / Shift+F10 context menu, Ctrl+Alt+I inspector, F9 dual, F10 Actions, Ctrl+, Preferences, Ctrl+D pin, Ctrl+Alt+S rail, Ctrl+. terminal, Ctrl+Alt+R Recents, Ctrl+Shift+O open with, F1 help | Done (Ctrl+Alt+S focuses the rail) |
| Alt+Enter properties | Done (`PropertiesDialog`, `core/properties.h`, after Files' `properties_dialog.rs`): up to 32 marked objects (Alt+Left/Right, Previous / Next); kind, apparent and allocated size, hard links, owner and group, symbolic and octal mode, link target (never followed), MIME type with the default application, volume, device and inode, modified / accessed / created / inode-change dates, the requested and verified paths, and the contents of a folder or the selection (counted off the UI thread, no links followed, one filesystem). The r/w/x bits (Tab, Space, clicks, "Allow executing as a program") apply with Enter or Apply permissions, by descriptor after checking device, inode, owner and mode; links, special objects, other owners and setuid/setgid/sticky stay read-only. Also in Actions and the context menu. Files' copy/export of the report: Next |
| Ctrl+Alt+D favorites and places | Done (`FavoritesHub`, after Files' `bookmarks_ui.rs`): favorites with their labels, the XDG places and the tab's Back / Forward history; typing filters by label, Enter opens, F2 edits the label (stored as the GTK bookmark label; the folder is never renamed; 1–96 bytes, one line), Delete removes a favorite (the folder stays), Ctrl+D adds the current folder, F5 reloads. Files' publish/ACK diagnostics (F1, F8) have no counterpart: Flo writes the bookmarks file directly |
| Ctrl+E star, Ctrl+Alt+E starred | Done (`core/starred.h`, Files' format in `$XDG_DATA_HOME/flo/starred`, up to 10,000 items): Ctrl+E stars the marked items (or the focused one) or unstars them when all are starred; a yellow star marks them in the List and Groups rows; Starred (the rail, Ctrl+Alt+E) lists them newest first with their folder, keeps missing ones as "unavailable", and its menu offers only Open, Open containing folder, Remove from Starred (Delete; the file is not touched) and Star. Removing a Recents entry (Files' Delete there): Next |
| Ctrl+Alt+B MC operation bar | Next |
| Nautilus shortcut profile | Next |

## Behavior

| Files | Flo |
|---|---|
| Automatic folder sizes for visible local folders, one helper, no links followed | Done (a worker thread over `core/tree_size.h`; folders of remote and trash locations are not measured) |
| The first entry is focused on arrival; the selection survives reloads | Done |
| Hidden files off by default, persisted | Done |
| Preferences: window normal / fullscreen, show thumbnails, automatic sizes, keep the rail open | Done; Files' border styles and alternate theme: Next |
| Sessions and saved per-folder views | Done: "Save / Forget this folder's view" (Preferences → Folder, `core/folder_views.h`, up to 256 folders) keeps a folder's view, sort and zoom and applies them whenever it opens; folders without one keep what the pane shows. `flo` without a folder restores the last session (`core/session.h`: tabs with history, view, sort, zoom, filter, marks and focus, the closed tabs, the active tab); it is saved when that window closes and with "Save session"; `flo PATH` and `flo --no-session` leave it alone |
| Directory sync and compare | Out |
| Size manager | Out |
| Advanced renamer (patterns, sequences, regex) | Out |
| Search facets (size and date filters) | Out |
| Translations | Partial: English and Spanish (Files' own wording) are complete; the other 48 catalogs show English for the strings added with the Files look until they are translated |

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
