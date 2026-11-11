// Saved folder views and the session file (Files' folder_views.rs and
// session_store.rs, in Flo's text formats).
#include "harness.h"

#include "core/folder_views.h"
#include "core/session.h"
#include "core/uri.h"

#include <sys/stat.h>

using namespace flo;
using namespace flo_test;

TEST(folder_views_round_trip_and_replace) {
    std::string dir = private_dir("views");
    std::string file = dir + "/folder-views";
    FolderViews v;
    CHECK(v.load(file)); // missing file: empty and writable
    CHECK(v.writable());
    CHECK(v.set({"/home/me/Pictures", ViewMode::Grid, SortMode::Newest, 132}));
    CHECK(v.set({"/home/me/a b\nc", ViewMode::List, SortMode::Size, 100}));
    CHECK(v.set({"/home/me/Pictures", ViewMode::Space, SortMode::Name, 500})); // replaced, zoom clamped
    CHECK(!v.set({"relative/path", ViewMode::List, SortMode::Name, 100}));
    CHECK(v.save(file));
    FolderViews w;
    CHECK(w.load(file));
    CHECK_EQ(w.all().size(), size_t(2));
    const FolderView *p = w.get("/home/me/Pictures");
    CHECK(p && p->view == ViewMode::Space && p->sort == SortMode::Name && p->zoom_percent == 180);
    const FolderView *q = w.get("/home/me/a b\nc");
    CHECK(q && q->view == ViewMode::List && q->sort == SortMode::Size);
    CHECK(!w.get("/home/me"));
    CHECK(w.forget("/home/me/a b\nc"));
    CHECK(!w.forget("/home/me/a b\nc"));
    CHECK(w.save(file));
    FolderViews x;
    CHECK(x.load(file));
    CHECK_EQ(x.all().size(), size_t(1));
}

TEST(folder_views_refuse_foreign_files_and_stop_at_256) {
    std::string dir = private_dir("views-bad");
    std::string file = write_file(dir + "/folder-views", "not ours\n");
    FolderViews v;
    CHECK(!v.load(file));
    CHECK(!v.writable());
    CHECK(!v.set({"/a", ViewMode::List, SortMode::Name, 100}));
    CHECK(!v.save(file));
    CHECK_EQ(read_file(file), std::string("not ours\n")); // kept as it was
    std::vector<FolderView> out;
    std::string why;
    CHECK(!FolderViews::parse("# Flo folder views, format 1\ngrid name 300 file:///a\n", out, why));
    CHECK(!FolderViews::parse("# Flo folder views, format 1\ngrid name 100 http://x/a\n", out, why));
    CHECK(FolderViews::parse("# Flo folder views, format 1\n\ngrid name 100 file:///a\n", out, why));
    FolderViews full;
    for (int i = 0; i < 256; ++i)
        CHECK(full.set({"/f/" + std::to_string(i), ViewMode::List, SortMode::Name, 100}));
    CHECK(!full.set({"/f/new", ViewMode::List, SortMode::Name, 100}));
    CHECK(full.set({"/f/7", ViewMode::Grid, SortMode::Name, 100})); // an update still fits
}

TEST(session_round_trip) {
    std::string dir = private_dir("session");
    std::string file = dir + "/session";
    Session s;
    TabState a;
    a.folder = "/home/me";
    a.back = {"/home"};
    a.forward = {"/home/me/Music", "/home/me/Music/Live 2026"};
    a.view = ViewMode::Grid;
    a.sort = SortMode::Size;
    a.zoom_percent = 115;
    a.filter = "*.jpg ext:png";
    a.selected = {"a b.jpg", "100%.png"};
    a.current = "a b.jpg";
    TabState b;
    b.folder = "/tmp/x y";
    s.tabs = {a, b};
    s.active = 1;
    s.closed = {"/srv"};
    CHECK(s.save(file));
    Session r;
    CHECK(Session::load(r, file));
    CHECK_EQ(r.tabs.size(), size_t(2));
    CHECK_EQ(r.active, 1);
    CHECK_EQ(r.tabs[0].folder, std::string("/home/me"));
    CHECK(r.tabs[0].back == a.back);
    CHECK(r.tabs[0].forward == a.forward);
    CHECK(r.tabs[0].view == ViewMode::Grid);
    CHECK(r.tabs[0].sort == SortMode::Size);
    CHECK_EQ(r.tabs[0].zoom_percent, 115);
    CHECK_EQ(r.tabs[0].filter, a.filter);
    CHECK(r.tabs[0].selected == a.selected);
    CHECK_EQ(r.tabs[0].current, a.current);
    CHECK_EQ(r.tabs[1].folder, std::string("/tmp/x y"));
    CHECK(r.closed == s.closed);
}

TEST(session_rejects_broken_files_whole) {
    Session out;
    std::string why;
    CHECK(!Session::parse("", out, why));
    CHECK(!Session::parse("# Flo session, format 1\nactive 0\n", out, why)); // no tabs
    CHECK(!Session::parse("# Flo session, format 1\nactive 2\ntab file:///a\n", out, why));
    CHECK(!Session::parse("# Flo session, format 1\nactive 0\nview grid\ntab file:///a\n", out, why));
    CHECK(!Session::parse("# Flo session, format 1\nactive 0\ntab file:///a\nselect a/b\n", out, why));
    CHECK(!Session::parse("# Flo session, format 1\nactive 0\ntab file:///a\nwhat ever\n", out, why));
    std::string nine = "# Flo session, format 1\nactive 0\n";
    for (int i = 0; i < 9; ++i)
        nine += "tab file:///t" + std::to_string(i) + "\n";
    CHECK(!Session::parse(nine, out, why));
    CHECK(Session::parse("# Flo session, format 1\nactive 0\ntab file:///a\n", out, why));
    CHECK(out.tabs.size() == 1 && out.tabs[0].view == ViewMode::Groups);
    std::string dir = private_dir("session-missing");
    CHECK(!Session::load(out, dir + "/none"));
    CHECK(out.tabs.empty());
}

#include "core/places.h"
#include "core/starred.h"

TEST(starred_toggle_round_trip_and_limits) {
    std::string dir = private_dir("starred");
    std::string file = dir + "/starred";
    Starred s;
    CHECK(s.load(file));
    CHECK_EQ(s.toggle({"/a", "/b c"}), 2);
    CHECK_EQ(s.toggle({"/a", "/d"}), 1);   // not all starred: star the rest
    CHECK_EQ(s.toggle({"/a", "/d"}), -2);  // all starred: unstar them
    CHECK(s.contains("/b c") && !s.contains("/a"));
    CHECK(s.add({"/x\ny"}));
    CHECK(s.save(file));
    Starred r;
    CHECK(r.load(file));
    CHECK(r.items() == std::vector<std::string>({"/b c", "/x\ny"}));
    std::vector<std::string> out;
    CHECK(Starred::parse("# x\n\nfile:///a\nfile:///a\nhttp://h/b\nfile:///c\r\n", out));
    CHECK(out == std::vector<std::string>({"/a", "/c"}));
    CHECK(!Starred::parse("\xff\xfe", out));
    std::string bad = write_file(dir + "/bad", "\xff\xfe junk\n");
    Starred b;
    CHECK(!b.load(bad));
    CHECK(!b.writable());
    CHECK_EQ(b.toggle({"/a"}), 0);
    CHECK(!b.save(bad));
    CHECK_EQ(read_file(bad), std::string("\xff\xfe junk\n"));
    Starred big;
    std::vector<std::string> many;
    for (int i = 0; i < 10000; ++i)
        many.push_back("/m/" + std::to_string(i));
    CHECK(big.add(many));
    CHECK(!big.add({"/one/more"}));
    CHECK_EQ(big.items().size(), size_t(10000));
}

TEST(favorite_labels_rewrite_only_their_line) {
    std::string dir = private_dir("favorites");
    std::string f1 = dir + "/a";
    std::string f2 = dir + "/b c";
    CHECK(::mkdir(f1.c_str(), 0700) == 0 && ::mkdir(f2.c_str(), 0700) == 0);
    std::string file = write_file(dir + "/bookmarks",
                                  "sftp://host/x Server\n" + path_to_uri(f1) + "\n" + path_to_uri(f2) + " Old\n");
    CHECK_EQ(places::relabel(f2, "  New label ", file), std::string());
    CHECK_EQ(places::relabel(f1, "Alpha", file), std::string());
    CHECK_EQ(read_file(file), "sftp://host/x Server\n" + path_to_uri(f1) + " Alpha\n" + path_to_uri(f2) + " New label\n");
    CHECK_EQ(places::relabel(f2, "", file), std::string());
    CHECK(read_file(file).find(path_to_uri(f2) + "\n") != std::string::npos);
    CHECK(!places::relabel(dir, "x", file).empty()); // not a favorite
    CHECK(!places::relabel(f1, std::string(97, 'x'), file).empty());
    CHECK(!places::relabel(f1, "a\tb", file).empty());
    CHECK_EQ(places::default_label(f2), std::string("b c"));
    CHECK_EQ(places::default_label("/" + std::string(120, 'z')).size(), size_t(96));
}
