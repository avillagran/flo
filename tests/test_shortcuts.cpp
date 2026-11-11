// Files' shortcut profiles (shortcuts.rs): the table, labels and remapping.
#include "harness.h"

#include "core/settings.h"
#include "core/shortcuts.h"

#include <set>

using namespace flo;
using namespace flo_test;

TEST(shortcut_profiles_follow_files_tables) {
    const auto F = ShortcutProfile::Files, N = ShortcutProfile::Nautilus;
    CHECK_EQ(chord_label(F, Cmd::Help), std::string("F1 / Ctrl+?"));
    CHECK_EQ(chord_label(F, Cmd::TabMoveLeft), std::string("Ctrl+Shift+PgUp"));
    CHECK_EQ(primary_chord(F, Cmd::ViewGrid), std::string("Ctrl+5"));
    CHECK_EQ(primary_chord(N, Cmd::ViewGrid), std::string("Ctrl+2"));
    CHECK_EQ(primary_chord(N, Cmd::ViewList), std::string("Ctrl+1"));
    CHECK_EQ(chord_label(N, Cmd::Refresh), std::string("F5 / Ctrl+R"));
    CHECK(chords(N, Cmd::CopyFiles).empty() && chords(N, Cmd::NewFile).empty());
    CHECK_EQ(chord_label(N, Cmd::Properties), std::string("Ctrl+I / Alt+Enter"));
    CHECK_EQ(primary_chord(N, Cmd::NewWindow), std::string("Ctrl+N"));
    CHECK(chords(F, Cmd::NewWindow).empty());
    CHECK_EQ(primary_chord(N, Cmd::Search), std::string("Ctrl+F"));
    CHECK_EQ(primary_chord(N, Cmd::Filter), std::string("Ctrl+Shift+F"));
    // Common rows stay in Nautilus.
    CHECK_EQ(primary_chord(N, Cmd::GoStarred), std::string("Ctrl+Alt+E"));
    // Aliases only where the command has chords.
    CHECK(aliases(F, Cmd::ZoomIn).size() == 1 && aliases(F, Cmd::ZoomIn)[0] == "Ctrl+=");
    CHECK(aliases(N, Cmd::NewFile).empty());
    // Operation bar: the F key when the profile has one.
    CHECK_EQ(function_key(F, Cmd::Preview), std::string("F3"));
    CHECK_EQ(function_key(N, Cmd::Preview), std::string("Space"));
    CHECK(function_key(N, Cmd::CopyFiles).empty());

    // No chord is bound twice within a profile (aliases included).
    for (auto p : {F, N}) {
        std::set<std::string> seen;
        for (int c = 0; c < int(Cmd::Count_); ++c) {
            std::vector<std::string> all = chords(p, Cmd(c));
            for (const auto &a : aliases(p, Cmd(c)))
                all.push_back(a);
            for (const auto &ch : all) {
                if (!seen.insert(ch).second)
                    fail(__FILE__, __LINE__, std::string("chord bound twice: ") + ch + " in " + profile_key(p));
            }
        }
    }
}

TEST(shortcut_profile_remaps_menu_chords_and_persists) {
    const auto F = ShortcutProfile::Files, N = ShortcutProfile::Nautilus;
    CHECK_EQ(remap_chord(F, "Alt+Enter"), std::string("Alt+Enter"));
    CHECK_EQ(remap_chord(N, "Alt+Enter"), std::string("Ctrl+I"));
    CHECK_EQ(remap_chord(N, "F5"), std::string(""));
    CHECK_EQ(remap_chord(N, "Ctrl+1"), std::string("Ctrl+3"));
    CHECK_EQ(remap_chord(N, "Ctrl+I"), std::string("Ctrl+Shift+I"));
    CHECK_EQ(remap_chord(N, "F3"), std::string("Space")); // Preview's second chord
    CHECK_EQ(remap_chord(N, "Ctrl+Shift+PgUp"), std::string("Ctrl+Shift+PgUp"));
    CHECK_EQ(remap_chord(N, "Esc"), std::string("Esc")); // not in the table

    CHECK(profile_from_key("nautilus") == N);
    CHECK(profile_from_key("emacs") == F);
    const std::string file = private_dir("shortcut-profile") + "/flo.conf";
    Settings s;
    s.shortcuts = N;
    CHECK(s.save(file));
    CHECK(Settings::load(file).shortcuts == N);
    CHECK(read_file(file).find("profile=nautilus") != std::string::npos ||
          read_file(file).find("profile = nautilus") != std::string::npos);
}
