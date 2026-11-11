// Natural sort, names, URIs, JSON, clipboard, settings, XDG parsing.
#include "harness.h"

#include "core/clipboard.h"
#include "core/entry.h"
#include "core/fsutil.h"
#include "core/json.h"
#include "core/natural_sort.h"
#include "core/settings.h"
#include "core/uri.h"
#include "core/xdg.h"

#include <algorithm>
#include <glib.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace flo;
using namespace flo_test;

TEST(natural_sort_orders_numbers_by_value) {
    std::vector<std::string> v{"file10", "File2", "file1", "file02", "Zeta", "alpha", "file2"};
    std::sort(v.begin(), v.end(), natural_less);
    const std::vector<std::string> want{"alpha", "file1", "File2", "file2", "file02", "file10", "Zeta"};
    CHECK(v == want);
    CHECK(natural_compare("a", "a") == 0);
    CHECK(natural_compare("img 9.png", "img 10.png") < 0);
    CHECK(natural_compare("Ábaco", "ábaco") < 0 || natural_compare("Ábaco", "ábaco") > 0); // total order
    CHECK(natural_compare("a", "ab") < 0);
    CHECK(natural_compare("x99999999999999999999", "x100000000000000000000") < 0);
}

TEST(folders_first_and_sort_modes) {
    std::vector<Entry> v(4);
    v[0].name = "b.txt"; v[0].size = 10; v[0].mtime_ns = 3; v[0].mime = "text/plain"; v[0].kind = Kind::Text;
    v[1].name = "a.png"; v[1].size = 30; v[1].mtime_ns = 1; v[1].mime = "image/png"; v[1].kind = Kind::Image;
    v[2].name = "Zdir"; v[2].is_dir = true; v[2].kind = Kind::Folder; v[2].mtime_ns = 0;
    v[3].name = "c.png"; v[3].size = 20; v[3].mtime_ns = 2; v[3].mime = "image/png"; v[3].kind = Kind::Image;
    auto names = [&] { std::string s; for (auto &e : v) s += e.name + ","; return s; };
    sort_entries(v, SortMode::Name);
    CHECK_EQ(names(), std::string("Zdir,a.png,b.txt,c.png,"));
    sort_entries(v, SortMode::Size);
    CHECK_EQ(names(), std::string("Zdir,a.png,c.png,b.txt,"));
    sort_entries(v, SortMode::Newest);
    CHECK_EQ(names(), std::string("Zdir,b.txt,c.png,a.png,"));
    sort_entries(v, SortMode::Oldest);
    CHECK_EQ(names(), std::string("Zdir,a.png,c.png,b.txt,"));
    sort_entries(v, SortMode::Kind);
    CHECK_EQ(names(), std::string("Zdir,a.png,c.png,b.txt,"));
    SortMode m;
    CHECK(parse_sort_mode("modified", m) && m == SortMode::Newest);
    CHECK(parse_sort_mode("newest", m) && m == SortMode::Newest);
    CHECK(!parse_sort_mode("bogus", m));
    CHECK_EQ(std::string(sort_mode_name(SortMode::Newest)), std::string("modified"));
}

TEST(free_name_follows_folio) {
    const std::string d = private_dir("freename");
    CHECK_EQ(free_name(d, "a.txt"), std::string("a.txt"));
    write_file(d + "/a.txt", "x");
    CHECK_EQ(free_name(d, "a.txt"), std::string("a (copy).txt"));
    write_file(d + "/a (copy).txt", "x");
    CHECK_EQ(free_name(d, "a.txt"), std::string("a (copy 2).txt"));
    write_file(d + "/photos.tar.gz", "x");
    CHECK_EQ(free_name(d, "photos.tar.gz"), std::string("photos (copy).tar.gz"));
    write_file(d + "/.bashrc", "x");
    CHECK_EQ(free_name(d, ".bashrc"), std::string(".bashrc (copy)"));
    ::mkdir((d + "/v1.2").c_str(), 0700);
    CHECK_EQ(free_name(d, "v1.2"), std::string("v1.2 (copy)"));
    CHECK_EQ(copy_name("x.png", false, 3), std::string("x (copy 3).png"));
}

TEST(paths_and_uris_match_glib) {
    const char *samples[] = {"/home/u/a b.txt", "/tmp/ñandú/#1?.png", "/x/100%/y", "/a/!$&'()*+,-.:=@_~", "/t/\t\n"};
    for (const char *s : samples) {
        gchar *g = g_filename_to_uri(s, nullptr, nullptr);
        CHECK_EQ(path_to_uri(s), std::string(g));
        CHECK_EQ(uri_to_path(g), std::string(s));
        g_free(g);
    }
    CHECK_EQ(uri_to_path("file://localhost/etc/x"), std::string("/etc/x"));
    CHECK_EQ(uri_to_path("sftp://host/x"), std::string());
    CHECK_EQ(uri_to_path("file:///bad%2"), std::string());
    CHECK_EQ(uri_to_path("file:///nul%00"), std::string());
    CHECK_EQ(uri_scheme("sftp://h/"), std::string("sftp"));
    CHECK_EQ(clean_path("/a//b/./c/../d/"), std::string("/a/b/d"));
    CHECK(path_inside("/a/b", "/a"));
    CHECK(!path_inside("/ab", "/a"));
    CHECK(path_inside("/a", "/"));
}

TEST(json_reader_and_quoting) {
    json::Value v;
    CHECK(json::parse(R"({"a":[1,2,{"b":"xé\n"}],"t":true,"n":null,"f":-1.5e2})", v));
    CHECK_EQ(v["a"][2]["b"].str(), std::string("x\xc3\xa9\n"));
    CHECK(v["t"].truthy());
    CHECK(v["f"].number == -150.0);
    CHECK(!json::parse("{\"a\":}", v));
    CHECK(!json::parse("[1,2", v));
    std::string deep(100, '[');
    CHECK(!json::parse(deep, v));
    CHECK_EQ(json::quote("a\"b\\\x01\xff"), std::string("\"a\\\"b\\\\\\u0001\xEF\xBF\xBD\""));
}

TEST(clipboard_payloads_round_trip) {
    const std::vector<std::string> paths{"/home/u/a b.txt", "/home/u/ñ"};
    const std::string g = clipboard::gnome_copied_files(paths, true);
    CHECK_EQ(g, std::string("cut\nfile:///home/u/a%20b.txt\nfile:///home/u/%C3%B1"));
    bool cut = false;
    CHECK(clipboard::parse_gnome_copied_files(g, cut) == paths);
    CHECK(cut);
    CHECK(clipboard::parse_gnome_copied_files("copy\nfile:///x\n", cut) == std::vector<std::string>{"/x"});
    CHECK(!cut);
    const std::string u = clipboard::uri_list(paths);
    CHECK_EQ(u, std::string("file:///home/u/a%20b.txt\r\nfile:///home/u/%C3%B1\r\n"));
    CHECK(clipboard::parse_uri_list("# comment\r\n" + u + "sftp://h/x\r\n") == paths);
    CHECK(clipboard::parse_any("text/plain", "/a\nfile:///b\n", cut) == (std::vector<std::string>{"/a", "/b"}));
    CHECK(clipboard::parse_any("text/plain", "hello world", cut).empty());
    CHECK_EQ(clipboard::plain_text(paths), std::string("/home/u/a b.txt\n/home/u/ñ"));
}

TEST(settings_round_trip_keeps_unknown_keys) {
    const std::string d = private_dir("settings");
    const std::string file = d + "/flo/flo.conf";
    Settings defaults = Settings::load(file);
    CHECK(defaults.view == ViewMode::Grid && defaults.sort == SortMode::Name && !defaults.show_hidden);
    ::mkdir((d + "/flo").c_str(), 0700);
    write_file(file, "# mine\n[view]\nmode=list\ncustom=keep\n[other]\nx=1\n");
    Settings s = Settings::load(file);
    CHECK(s.view == ViewMode::List);
    s.sort = SortMode::Oldest;
    s.show_hidden = true;
    s.window_width = 1500;
    CHECK(s.save(file));
    const std::string text = read_file(file);
    CHECK(text.find("# mine") != std::string::npos);
    CHECK(text.find("custom=keep") != std::string::npos);
    CHECK(text.find("[other]\nx=1") != std::string::npos);
    Settings again = Settings::load(file);
    CHECK(again.sort == SortMode::Oldest && again.show_hidden && again.window_width == 1500 &&
          again.view == ViewMode::List);
    struct stat st;
    CHECK(::stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0644);
    // Bad values are ignored, not fatal.
    Ini ini;
    ini.parse("[window]\nwidth=-5\n[view]\nsort=nope\n");
    Settings b;
    b.read_from(ini);
    CHECK(b.window_width == 1280 && b.sort == SortMode::Name);
    // The default path honours XDG_CONFIG_HOME (the private one here).
    CHECK(path_inside(Settings::path(), getenv("FLO_TEST_RUN")));
}

TEST(user_dirs_parsing) {
    auto dirs = xdg::parse_user_dirs("# c\nXDG_DOWNLOAD_DIR=\"$HOME/Down loads\"\nXDG_MUSIC_DIR=\"/abs/m\"\nbad\n"
                                     "XDG_DESKTOP_DIR=\"$HOME/\"\n", "/h");
    CHECK_EQ(dirs.size(), static_cast<std::size_t>(3));
    CHECK_EQ(dirs[0].first, std::string("DOWNLOAD"));
    CHECK_EQ(dirs[0].second, std::string("/h/Down loads"));
    CHECK_EQ(dirs[1].second, std::string("/abs/m"));
    CHECK_EQ(dirs[2].second, std::string("/h"));
}

TEST(mountinfo_parsing) {
    const std::string text =
        "22 1 259:2 / / rw,relatime shared:1 - btrfs /dev/nvme0n1p2 rw\n"
        "40 22 8:17 / /run/media/u/My\\040Disk rw,nosuid - exfat /dev/sdb1 rw\n"
        "41 22 0:5 / /proc rw - proc proc rw\n";
    auto rows = parse_mountinfo(text);
    CHECK_EQ(rows.size(), static_cast<std::size_t>(3));
    CHECK_EQ(rows[1].root_path, std::string("/run/media/u/My Disk"));
    CHECK_EQ(rows[1].fs_type, std::string("exfat"));
    CHECK_EQ(rows[1].source, std::string("/dev/sdb1"));
    CHECK(is_pseudo_fs("proc") && !is_pseudo_fs("ext4"));
    CHECK(is_remote_fs("fuse.sshfs") && !is_remote_fs("fuse.fuseblk") && is_remote_fs("nfs4"));
}

TEST(atomic_write_and_capped_read) {
    const std::string d = private_dir("atomic");
    CHECK(atomic_write_file(d + "/f", "hello", 0600));
    CHECK(atomic_write_file(d + "/f", "world", 0600));
    CHECK_EQ(read_file(d + "/f"), std::string("world"));
    std::string out;
    CHECK(!read_file_capped(d + "/f", 3, out));
    CHECK(::symlink("f", (d + "/link").c_str()) == 0);
    CHECK(!read_file_capped(d + "/link", 100, out, false));
    CHECK(read_file_capped(d + "/link", 100, out, true));
}
