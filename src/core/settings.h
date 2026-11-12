// Flo's settings: $XDG_CONFIG_HOME/flo/flo.conf, a simple INI file written
// atomically. Unknown keys and comments are kept on save.
#pragma once

#include "core/entry.h"
#include "core/shortcuts.h"

#include <map>
#include <string>
#include <vector>

namespace flo {

class Ini {
public:
    bool parse(const std::string &text);
    std::string serialize() const;

    std::string get(const std::string &section, const std::string &key, const std::string &fallback = {}) const;
    void set(const std::string &section, const std::string &key, const std::string &value);
    bool has(const std::string &section, const std::string &key) const;

private:
    struct Line {
        std::string section; // section this line belongs to
        std::string key;     // empty for comments, blank lines and headers
        std::string raw;     // the text as read (for non-key lines)
        std::string value;
    };
    std::vector<Line> lines_;
};

// Files' views: Groups (Ctrl+1), List (2), Columns (3), Space (4), Grid (5).
enum class ViewMode { Grid, List, Columns, Groups, Space };
const char *view_mode_name(ViewMode m);
bool parse_view_mode(const std::string &s, ViewMode &out);

struct Settings {
    ViewMode view = ViewMode::Groups; // Files opens in Groups
    SortMode sort = SortMode::Name;
    bool show_hidden = false;
    bool details_pane = true;  // Files' inspector
    bool rail_pinned = false;  // the rail stays expanded
    bool auto_sizes = true;    // measure visible folders (Files' default)
    bool thumbnails = true;    // pictures and videos show their thumbnails
    bool operation_bar = false; // Files' MC-style F-key bar (Ctrl+Alt+B)
    ShortcutProfile shortcuts = ShortcutProfile::Files; // [keys] profile: files or nautilus
    int window_width = 1100, window_height = 700; // Files' first size
    bool window_maximized = false;
    int thumbnail_size = 256; // requested pixels (Folio: large)
    // Files' "Interface borders" (omarchy: Hyprland's rounding, square,
    // rounded) and "Theme" (the live Omarchy theme or Tokyo Night as a
    // local view), [view] corners and theme.
    std::string corners = "omarchy";
    bool local_theme = false;
    // Files' rail_order: the places and devices dragged to a new place in
    // the rail ([rail] order, keys from core/rail_order.h).
    std::vector<std::string> rail_order;

    static std::string path(); // $XDG_CONFIG_HOME/flo/flo.conf
    // Missing file or keys give the defaults; bad values are ignored.
    static Settings load(const std::string &file = path());
    bool save(const std::string &file = path()) const;

    void read_from(const Ini &ini);
    void write_to(Ini &ini) const;
};

} // namespace flo
