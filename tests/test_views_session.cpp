// Saved folder views and the session file (Files' folder_views.rs and
// session_store.rs, in Flo's text formats).
#include "harness.h"

#include "core/folder_views.h"
#include "core/session.h"

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
