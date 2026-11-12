// Files' rail: place and device order (sidebar_order.rs), bookmark moves
// in the GTK file, and the Network section's locations (network.rs).
#include "harness.h"

#include "core/fsutil.h"
#include "core/network.h"
#include "core/places.h"
#include "core/rail_order.h"
#include "core/settings.h"
#include "core/uri.h"

#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace flo;
using namespace flo_test;

static std::vector<std::string> list_names(const std::string &dir) {
    std::vector<std::string> out;
    if (DIR *d = ::opendir(dir.c_str())) {
        while (dirent *e = ::readdir(d))
            out.push_back(e->d_name);
        ::closedir(d);
    }
    return out;
}

TEST(rail_order_moves_keep_absent_rows_and_other_sections) {
    // Files' move_retains_absent_rows_and_other_sections.
    const std::vector<std::string> saved = {"p:a", "p:missing", "d:x", "p:b"};
    const std::vector<std::string> visible = {"p:a", "p:b", "p:c"};
    std::vector<std::string> out;
    CHECK(rail_order::moved(saved, visible, "p:c", 0, out));
    CHECK(out == (std::vector<std::string>{"p:c", "p:missing", "d:x", "p:a", "p:b"}));
    CHECK(rail_order::ordered_indices(visible, out) == (std::vector<std::size_t>{2, 0, 1}));
    std::vector<std::string> again;
    CHECK(!rail_order::moved(out, visible, "p:missing", 0, again));
    CHECK(!rail_order::moved({"p:a", "p:a"}, visible, "p:a", 1, again));
    // Distinct raw paths never share a key; a place key carries its label.
    CHECK(rail_order::path_key("place", "/\xff") != rail_order::path_key("place", "/\xfe"));
    CHECK_EQ(rail_order::path_key("place", "/a"), std::string("place:2f61"));
    CHECK_EQ(rail_order::place_key("/a", "Home"), std::string("place:2f6100486f6d65"));
    // The settings value round-trips and drops malformed or repeated keys.
    CHECK(rail_order::parse(rail_order::serialize({"place:2f61", "dev:2f62"})) ==
          (std::vector<std::string>{"place:2f61", "dev:2f62"}));
    CHECK(rail_order::parse("place:2f61,place:2f61,../bad,place:2f6,dev:2F62") ==
          (std::vector<std::string>{"place:2f61"}));
    // Kept in the settings as [rail] order.
    const std::string d = private_dir("rail-settings");
    Settings s;
    s.rail_order = {"place:2f61", "dev:2f62"};
    CHECK(s.save(d + "/flo.conf"));
    CHECK(read_file(d + "/flo.conf").find("order=place:2f61,dev:2f62") != std::string::npos);
    CHECK(Settings::load(d + "/flo.conf").rail_order == s.rail_order);
}

TEST(bookmarks_move_only_their_own_line) {
    const std::string d = private_dir("bookmark-move");
    const std::string file = d + "/gtk-3.0/bookmarks";
    ::mkdir((d + "/gtk-3.0").c_str(), 0700);
    for (const char *n : {"a", "b", "c"})
        ::mkdir((d + "/" + n).c_str(), 0700);
    auto uri = [&](const char *n) { return path_to_uri(d + "/" + n); };
    write_file(file, uri("a") + " A\nsftp://box/ Box\n" + uri("b") + "\n" + uri("missing") + " Gone\n" + uri("c") +
                         " C\n");
    auto names = [&] {
        std::string out;
        for (const auto &p : places::pinned(places::read_bookmarks(file)))
            out += p.name + ",";
        return out;
    };
    CHECK_EQ(names(), std::string("A,b,C,"));
    // A to the end: after C; the remote and missing lines stay where they were.
    CHECK_EQ(places::move_bookmark(d + "/a", 2, file), std::string());
    CHECK_EQ(names(), std::string("b,C,A,"));
    CHECK_EQ(read_file(file), "sftp://box/ Box\n" + uri("b") + "\n" + uri("missing") + " Gone\n" + uri("c") + " C\n" +
                                  uri("a") + " A\n");
    // C first: before b.
    CHECK_EQ(places::move_bookmark(d + "/c", 0, file), std::string());
    CHECK_EQ(names(), std::string("C,b,A,"));
    CHECK(read_file(file).find("sftp://box/ Box\n") == 0);
    // Where it already is: nothing written; unknown paths are refused.
    CHECK_EQ(places::move_bookmark(d + "/b", 1, file), std::string());
    CHECK(!places::move_bookmark(d + "/missing", 0, file).empty());
    CHECK_EQ(names(), std::string("C,b,A,"));
}

TEST(network_uris_are_checked_like_files) {
    NetworkLocation l;
    CHECK_EQ(parse_network_uri("smb://ana@nas/fotos", l), std::string());
    CHECK_EQ(l.uri, std::string("smb://ana@nas/fotos"));
    CHECK_EQ(l.user, std::string("ana"));
    CHECK(l.inline_password.empty());
    // The password leaves the URI; a domain;user keeps only the user.
    CHECK_EQ(parse_network_uri("  FTP://WORK%3Bbo:s%40cret@files.example:2121  ", l), std::string());
    CHECK_EQ(l.uri, std::string("ftp://WORK%3Bbo@files.example:2121/"));
    CHECK_EQ(l.inline_password, std::string("s@cret"));
    CHECK_EQ(parse_network_uri("smb://dom;ana@nas/x", l), std::string());
    CHECK_EQ(l.user, std::string("ana"));
    CHECK_EQ(parse_network_uri("smb:///share", l), std::string("Missing the server: smb://server/share"));
    CHECK_EQ(parse_network_uri("smb://:445/x", l), std::string("Missing the server: smb://server/share"));
    CHECK(!parse_network_uri("http://nas/", l).empty());
    CHECK(!parse_network_uri("smb://nas/a b", l).empty());
    CHECK(!parse_network_uri("smb://nas/" + std::string(2100, 'x'), l).empty());
    CHECK(is_network_uri("ssh://box/") && is_network_uri("DAVS://h/") && !is_network_uri("trash:///"));
}

TEST(gvfs_bridge_names_become_rail_rows) {
    const std::string root = "/run/user/1/gvfs";
    auto named = [&](const std::string &n) { return gvfs_share(root + "/" + n, n); };
    GvfsShare s = named("smb-share:server=nas,share=fotos,user=ana,domain=WORK");
    CHECK_EQ(s.protocol, std::string("SMB"));
    CHECK_EQ(s.uri, std::string("smb://WORK;ana@nas/fotos"));
    CHECK(s.title == "nas" && s.share == "fotos" && s.user == "ana");
    s = named("sftp:host=h,port=2222,user=ana");
    CHECK_EQ(s.uri, std::string("sftp://ana@h:2222/"));
    CHECK(s.protocol == "SFTP" && s.title == "h" && s.share.empty());
    s = named("dav:host=cloud,ssl=true,prefix=%2Fremote.php%2Fdav%2F");
    CHECK_EQ(s.uri, std::string("davs://cloud/remote.php/dav/"));
    CHECK_EQ(s.title, std::string("cloud/remote.php/dav"));
    s = named("nfs:host=srv,prefix=%2Fexport");
    CHECK(s.uri == "nfs://srv/export" && s.title == "srv:/export");
    s = named("afp-volume:host=mac,volume=Media");
    CHECK(s.uri == "afp://mac/Media" && s.share == "Media");
    s = named("mtp:host=phone");
    CHECK(s.protocol.empty() && s.title == "mtp:host=phone" && s.uri == root + "/mtp:host=phone");
    s = named("smb-share:server=nas");
    CHECK(s.protocol.empty() && s.uri == s.path);
}

TEST(gvfs_scan_lists_and_fake_disconnect_moves_aside) {
    const std::string d = private_dir("gvfs");
    for (const char *n : {"sftp:host=zed", "smb-share:server=nas,share=fotos", ".hidden"})
        CHECK(::mkdir((d + "/" + n).c_str(), 0700) == 0);
    write_file(d + "/sftp:host=zed/keep.txt", "kept");
    auto list = scan_gvfs(d);
    CHECK_EQ(list.size(), size_t(2));
    CHECK(list[0].share == "fotos" && list[1].title == "zed");
    CHECK(scan_gvfs(d + "/none").empty());
    // Under the fake GVfs a disconnect renames the bridge folder; its
    // contents stay as they were.
    CHECK(::setenv("FLO_TEST_FAKE_GVFS", d.c_str(), 1) == 0);
    CHECK_EQ(gvfs_dir(), d);
    CHECK_EQ(unmount_location(list[1]), std::string());
    CHECK(!exists_nofollow(d + "/sftp:host=zed"));
    CHECK_EQ(scan_gvfs(d).size(), size_t(1));
    bool kept = false;
    for (const auto &e : list_names(d))
        if (e.compare(0, 24, ".unmounted-sftp:host=zed") == 0)
            kept = read_file(d + "/" + e + "/keep.txt") == "kept";
    CHECK(kept);
    GvfsShare outside;
    outside.path = private_dir("gvfs-outside");
    CHECK(!unmount_location(outside).empty());
    CHECK(is_dir(outside.path));
    ::unsetenv("FLO_TEST_FAKE_GVFS");
}
