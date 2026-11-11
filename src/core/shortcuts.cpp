#include "core/shortcuts.h"

#include <array>
#include <utility>

namespace flo {

namespace {

using Row = std::pair<Cmd, std::vector<std::string>>;

// Files' COMMON table, in its order.
const std::vector<Row> &common() {
    static const std::vector<Row> rows = {
        {Cmd::Commands, {"Ctrl+Shift+P"}},
        {Cmd::Help, {"F1", "Ctrl+?"}},
        {Cmd::TabNew, {"Ctrl+T"}},
        {Cmd::TabClose, {"Ctrl+W"}},
        {Cmd::TabReopen, {"Ctrl+Shift+T"}},
        {Cmd::TabNext, {"Ctrl+Tab", "Ctrl+PageDown"}},
        {Cmd::TabPrev, {"Ctrl+Shift+Tab", "Ctrl+PageUp"}},
        {Cmd::TabMoveLeft, {"Ctrl+Shift+PageUp"}},
        {Cmd::TabMoveRight, {"Ctrl+Shift+PageDown"}},
        {Cmd::OperationBar, {"Ctrl+Alt+B"}},
        {Cmd::FocusRail, {"Ctrl+Alt+S"}},
        {Cmd::FocusCycle, {}},
        {Cmd::DualPane, {"F9"}},
        {Cmd::RailToggle, {}},
        {Cmd::Search, {"Ctrl+Shift+F"}},
        {Cmd::Filter, {"Ctrl+F"}},
        {Cmd::SizeFocused, {"Ctrl+Space"}},
        {Cmd::SizesListed, {"Ctrl+Shift+Space"}},
        {Cmd::BookmarkAdd, {"Ctrl+D"}},
        {Cmd::Bookmarks, {"Ctrl+Alt+D"}},
        {Cmd::MarkAdvance, {"Insert"}},
        {Cmd::MarkToggle, {"Ctrl+Insert"}},
        {Cmd::OpenWith, {"Ctrl+Shift+O"}},
        {Cmd::Terminal, {"Ctrl+."}},
        {Cmd::CopyPath, {"Ctrl+Alt+C"}},
        {Cmd::Preview, {"Space", "F3"}},
        {Cmd::Trash, {"Delete", "F8"}},
        {Cmd::DeletePermanently, {"Shift+Delete"}},
        {Cmd::Breadcrumbs, {"Alt+Down"}},
        {Cmd::Open, {"Enter"}},
        {Cmd::OpenInTab, {"Ctrl+Enter"}},
        {Cmd::OpenInBackground, {"Shift+Enter"}},
        {Cmd::Back, {"Alt+Left"}},
        {Cmd::Forward, {"Alt+Right"}},
        {Cmd::Parent, {"Backspace", "Alt+Up"}},
        {Cmd::Home, {"Alt+Home"}},
        {Cmd::Properties, {"Alt+Enter"}},
        {Cmd::ClipboardCopy, {"Ctrl+C"}},
        {Cmd::ClipboardCut, {"Ctrl+X"}},
        {Cmd::ClipboardPaste, {"Ctrl+V"}},
        {Cmd::NativePaste, {"Ctrl+Shift+V"}},
        {Cmd::Hidden, {"Ctrl+H"}},
        {Cmd::Refresh, {"Ctrl+R"}},
        {Cmd::ContextMenu, {"Menu", "Shift+F10"}},
        {Cmd::Inspector, {"Ctrl+Alt+I"}},
        {Cmd::Actions, {"F10"}},
        {Cmd::Settings, {"Ctrl+,"}},
        {Cmd::CopyFiles, {"F5"}},
        {Cmd::MoveFiles, {"F6"}},
        {Cmd::Rename, {"F2"}},
        {Cmd::NewFolder, {"F7", "Ctrl+Shift+N"}},
        {Cmd::NewFile, {"Ctrl+N"}},
        {Cmd::NewWindow, {}},
        {Cmd::Undo, {"Ctrl+Z"}},
        {Cmd::Redo, {"Ctrl+Shift+Z"}},
        {Cmd::Location, {"Ctrl+L"}},
        {Cmd::ViewGroups, {"Ctrl+1"}},
        {Cmd::ViewList, {"Ctrl+2"}},
        {Cmd::ViewColumns, {"Ctrl+3"}},
        {Cmd::ViewSpace, {"Ctrl+4"}},
        {Cmd::ViewGrid, {"Ctrl+5"}},
        {Cmd::ZoomIn, {"Ctrl++"}},
        {Cmd::ZoomOut, {"Ctrl+-"}},
        {Cmd::ZoomReset, {"Ctrl+0"}},
        {Cmd::SelectAll, {"Ctrl+A"}},
        {Cmd::SelectFiltered, {"Ctrl+Shift+A"}},
        {Cmd::ClearSelection, {"Ctrl+Shift+D"}},
        {Cmd::InvertSelection, {"Ctrl+I"}},
        {Cmd::SortMenu, {"Ctrl+Shift+S"}},
        // Free in both profiles (GNOME Files binds none of them).
        {Cmd::GoRecent, {"Ctrl+Alt+R"}},
        {Cmd::GoStarred, {"Ctrl+Alt+E"}},
        {Cmd::StarToggle, {"Ctrl+E"}},
        {Cmd::OpenContaining, {"Ctrl+Alt+O"}},
    };
    return rows;
}

// Files' NAUTILUS table: GNOME Files (Nautilus) conventions.
const std::vector<Row> &nautilus() {
    static const std::vector<Row> rows = {
        {Cmd::Refresh, {"F5", "Ctrl+R"}},
        {Cmd::CopyFiles, {}},
        {Cmd::MoveFiles, {}},
        {Cmd::FocusCycle, {"F6"}},
        {Cmd::RailToggle, {"F9"}},
        {Cmd::DualPane, {"F3"}},
        {Cmd::Preview, {"Space"}},
        {Cmd::ViewList, {"Ctrl+1"}},
        {Cmd::ViewGrid, {"Ctrl+2"}},
        {Cmd::ViewGroups, {"Ctrl+3"}},
        {Cmd::ViewColumns, {"Ctrl+4"}},
        {Cmd::ViewSpace, {"Ctrl+5"}},
        {Cmd::Properties, {"Ctrl+I", "Alt+Enter"}},
        {Cmd::InvertSelection, {"Ctrl+Shift+I"}},
        {Cmd::NewWindow, {"Ctrl+N"}},
        {Cmd::NewFile, {}},
        {Cmd::NewFolder, {"Ctrl+Shift+N", "F7"}},
        {Cmd::MarkToggle, {"Ctrl+Space", "Ctrl+Insert"}},
        {Cmd::SizeFocused, {}},
        {Cmd::Open, {"Enter", "Alt+Down"}},
        {Cmd::Breadcrumbs, {}},
        {Cmd::Search, {"Ctrl+F"}},
        {Cmd::Filter, {"Ctrl+Shift+F"}},
    };
    return rows;
}

const std::vector<Row> &alias_rows() {
    static const std::vector<Row> rows = {
        {Cmd::ZoomIn, {"Ctrl+="}},
        {Cmd::Preview, {"Shift+Space"}},
    };
    return rows;
}

const std::vector<std::string> *find(const std::vector<Row> &rows, Cmd c) {
    for (const auto &r : rows)
        if (r.first == c)
            return &r.second;
    return nullptr;
}

std::string printed(std::string chord) {
    for (auto [from, to] : std::array<std::pair<const char *, const char *>, 2>{
             {{"PageUp", "PgUp"}, {"PageDown", "PgDn"}}}) {
        auto at = chord.find(from);
        if (at != std::string::npos)
            chord.replace(at, std::string(from).size(), to);
    }
    return chord;
}

} // namespace

const char *profile_key(ShortcutProfile p) { return p == ShortcutProfile::Nautilus ? "nautilus" : "files"; }

ShortcutProfile profile_from_key(const std::string &key) {
    return key == "nautilus" ? ShortcutProfile::Nautilus : ShortcutProfile::Files;
}

const std::vector<std::string> &chords(ShortcutProfile p, Cmd c) {
    static const std::vector<std::string> none;
    if (p == ShortcutProfile::Nautilus)
        if (const auto *row = find(nautilus(), c))
            return *row;
    const auto *row = find(common(), c);
    return row ? *row : none;
}

std::vector<std::string> aliases(ShortcutProfile p, Cmd c) {
    if (chords(p, c).empty())
        return {};
    const auto *row = find(alias_rows(), c);
    if (!row)
        return {};
    return *row;
}

std::string chord_label(ShortcutProfile p, Cmd c) {
    std::string out;
    for (const auto &ch : chords(p, c))
        out += (out.empty() ? "" : " / ") + printed(ch);
    return out;
}

std::string primary_chord(ShortcutProfile p, Cmd c) {
    const auto &all = chords(p, c);
    return all.empty() ? std::string() : printed(all.front());
}

std::string function_key(ShortcutProfile p, Cmd c) {
    const auto &all = chords(p, c);
    for (const auto &ch : all)
        if (ch.size() >= 2 && ch[0] == 'F' && ch[1] >= '0' && ch[1] <= '9')
            return ch;
    return all.empty() ? std::string() : printed(all.front());
}

std::string remap_chord(ShortcutProfile p, const std::string &files_chord) {
    if (p == ShortcutProfile::Files || files_chord.empty())
        return files_chord;
    // The command whose Files chords hold this one, its primary first.
    for (int pass = 0; pass < 2; ++pass)
        for (const auto &row : common()) {
            const auto &all = row.second;
            for (size_t i = 0; i < all.size(); ++i) {
                if ((pass == 0) != (i == 0))
                    continue;
                if (printed(all[i]) == files_chord || all[i] == files_chord)
                    return primary_chord(p, row.first);
            }
        }
    return files_chord;
}

} // namespace flo
