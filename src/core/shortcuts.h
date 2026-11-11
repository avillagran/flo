// Files' keyboard shortcut profiles (files-rust crates/ui shortcuts.rs):
// one table of chords per command. The Files profile is Files' own keys;
// the Nautilus profile moves some of them to GNOME Files' conventions
// (F5 reloads, Ctrl+1/2 List/Grid, Ctrl+I properties, Ctrl+N a new
// window...). Commands whose chord moves away stay reachable from the
// menus and the command palette.
//
// Chords are toolkit key specs ("Ctrl+Shift+F", "Alt+Down", "Space",
// "Ctrl+PageUp"), most prominent first. Window keys, the help, menus,
// the toolbar and the operation bar all print chord_label(), so what is
// advertised always matches the active profile.
#pragma once

#include <string>
#include <vector>

namespace flo {

enum class ShortcutProfile { Files, Nautilus };

// "files" / "nautilus"; anything else reads as Files.
const char *profile_key(ShortcutProfile p);
ShortcutProfile profile_from_key(const std::string &key);

enum class Cmd {
    Commands, Help, TabNew, TabClose, TabReopen, TabNext, TabPrev, TabMoveLeft, TabMoveRight,
    OperationBar, FocusRail, FocusCycle, DualPane, RailToggle, Search, Filter, SizeFocused, SizesListed,
    BookmarkAdd, Bookmarks, MarkAdvance, MarkToggle, OpenWith, Terminal, CopyPath, Preview, Trash,
    DeletePermanently, Breadcrumbs, Open, OpenInTab, OpenInBackground, Back, Forward, Parent, Home,
    Properties, ClipboardCopy, ClipboardCut, ClipboardPaste, NativePaste, Hidden, Refresh, ContextMenu,
    Inspector, Actions, Settings, CopyFiles, MoveFiles, Rename, NewFolder, NewFile, NewWindow, Undo, Redo,
    Location, ViewGroups, ViewList, ViewColumns, ViewSpace, ViewGrid, ZoomIn, ZoomOut, ZoomReset,
    SelectAll, SelectFiltered, ClearSelection, InvertSelection, SortMenu, GoRecent, GoStarred,
    StarToggle, OpenContaining,
    Count_
};

// The printed chords of a command in a profile (the profile's own row
// wins over the common one), most prominent first; empty when none.
const std::vector<std::string> &chords(ShortcutProfile p, Cmd c);
// Layout variants matched but never printed ("Ctrl+=" for Ctrl++), only
// for commands that have chords in the profile.
std::vector<std::string> aliases(ShortcutProfile p, Cmd c);
// "F1 / Ctrl+?" (PageUp/PageDown printed as PgUp/PgDn); "" when none.
std::string chord_label(ShortcutProfile p, Cmd c);
// The first chord only, for menu accelerators; "" when none.
std::string primary_chord(ShortcutProfile p, Cmd c);
// Files' operation bar: a button's function-key chord when the profile
// has one (MC style), otherwise its first chord, otherwise "".
std::string function_key(ShortcutProfile p, Cmd c);

// A chord printed in the Files profile, as the active profile prints the
// same command ("Alt+Enter" -> "Ctrl+I" in Nautilus, "F5" -> "" where copy
// has no key). Unknown chords and the Files profile come back unchanged.
std::string remap_chord(ShortcutProfile p, const std::string &files_chord);

} // namespace flo
