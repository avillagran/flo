// The last session, as Files restores it (session_store.rs): the tabs of a
// window with their folder, Back/Forward history, view, sort, Grid zoom,
// filter and selection, the closed tabs (Ctrl+Shift+T) and the active tab.
// It is saved when a window closes (and with "Save session") and restored
// when Flo starts without a folder to open (`flo`, not `flo PATH` or
// `flo --no-session`). The dual pane and hidden files are not part of it.
//
// $XDG_STATE_HOME/flo/session, a text file written atomically:
//
//   # Flo session, format 1
//   active 1
//   tab file:///home/me
//   view grid
//   sort name
//   zoom 115
//   back file:///home
//   forward file:///home/me/Music
//   filter *.jpg
//   select a%20b.jpg
//   closed file:///tmp
//
// Lines after a `tab` line belong to that tab. Paths and names are
// percent-encoded; anything malformed rejects the whole file (the session
// starts at home and the file is left as it is).
#pragma once

#include "core/entry.h"
#include "core/settings.h"

#include <string>
#include <vector>

namespace flo {

struct TabState {
    std::string folder;
    std::vector<std::string> back, forward; // oldest first
    ViewMode view = ViewMode::Groups;
    SortMode sort = SortMode::Name;
    int zoom_percent = 100;
    std::string filter;
    std::vector<std::string> selected; // names in the folder
    std::string current;               // the focused name
};

struct Session {
    static constexpr std::size_t kMaxTabs = 8, kMaxClosed = 16, kMaxHistory = 64, kMaxSelected = 4096,
                                 kMaxBytes = 1u << 20;
    std::vector<TabState> tabs;
    std::vector<std::string> closed;
    int active = 0;

    static std::string path(); // $XDG_STATE_HOME/flo/session
    std::string serialize() const;
    // False (and `why`) for anything that is not a complete format 1 file.
    static bool parse(const std::string &text, Session &out, std::string &why);
    // A missing or bad file gives false and an empty session.
    static bool load(Session &out, const std::string &file = path());
    bool save(const std::string &file = path()) const;
};

} // namespace flo
