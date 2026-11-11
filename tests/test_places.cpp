// Places, bookmarks, recents, network parsing, and a source guard.
#include "harness.h"

#include "core/dbus_service.h"
#include "core/event_queue.h"
#include "core/fsutil.h"
#include "core/mounter.h"
#include "core/network.h"
#include "core/places.h"
#include "core/recents.h"
#include "core/uri.h"
#include "core/xdg.h"

#include <cstdlib>
#include <functional>
#include <sys/stat.h>
#include <unistd.h>

using namespace flo;
using namespace flo_test;

TEST(home_places_with_roles) {
    const std::string home = xdg::home(); // private in tests
    CHECK(path_inside(home, std::getenv("FLO_TEST_RUN")));
    for (const char *n : {"Downloads", "omarchy", "Dropbox", "Work", "Music", "projects 10", "projects 9"})
        ::mkdir((home + "/" + n).c_str(), 0700);
    write_file(xdg::config_home() + "/user-dirs.dirs", "XDG_MUSIC_DIR=\"$HOME/Music\"\nXDG_DOWNLOAD_DIR=\"$HOME/Downloads\"\n");
    auto p = places::home_places(home, "synced");
    std::string names, icons;
    for (auto &x : p) {
        names += x.name + ",";
        icons += x.icon + ",";
    }
    CHECK(names.find("Downloads,Dropbox,Music,Omarchy,projects 9,projects 10,Work,") != std::string::npos);
    CHECK(icons.find("downloads,dropbox,music,folder,folder,folder,folder,") != std::string::npos);
    for (auto &x : p)
        if (x.icon == "dropbox")
            CHECK_EQ(x.status, std::string("synced"));
    auto v = places::views();
    CHECK(v.size() == 2 && v[1].path == xdg::data_home() + "/Trash/files");
}

TEST(bookmarks_pin_unpin_keep_remote) {
    const std::string d = private_dir("bookmarks");
    const std::string file = d + "/gtk-3.0/bookmarks";
    ::mkdir((d + "/gtk-3.0").c_str(), 0700);
    ::mkdir((d + "/proj x").c_str(), 0700);
    ::mkdir((d + "/other").c_str(), 0700);
    write_file(file, "sftp://box/home/u Box\nfile://" + percent_encode_path(d + "/proj x") + " Project\nsmb://nas/share\n");
    auto bm = places::read_bookmarks(file);
    CHECK_EQ(bm.size(), static_cast<std::size_t>(3));
    auto pinned = places::pinned(bm, "/nonexistent-home");
    CHECK(pinned.size() == 1 && pinned[0].name == "Project" && pinned[0].path == d + "/proj x");
    auto remote = places::remote_bookmarks(bm);
    CHECK(remote.size() == 2 && remote[0].name == "Box" && remote[1].uri == "smb://nas/share");
    CHECK_EQ(places::pin(d + "/other", "/nonexistent-home", file), std::string());
    CHECK(places::is_pinned(d + "/other", file));
    CHECK_EQ(places::pin(d + "/other", "/nonexistent-home", file), std::string()); // no duplicate
    CHECK(!places::pin(d + "/missing", "/nonexistent-home", file).empty());
    CHECK(!places::pin(d + "/other", d, file).empty()); // already under Home
    CHECK(places::unpin(d + "/proj x", file));
    const std::string text = read_file(file);
    CHECK(text.find("sftp://box/home/u Box") != std::string::npos);
    CHECK(text.find("smb://nas/share") != std::string::npos);
    CHECK(text.find("proj%20x") == std::string::npos);
    CHECK(places::read_bookmarks(file).size() == 3);
    CHECK(path_inside(places::bookmarks_path(), std::getenv("FLO_TEST_RUN")));
}

// The metadata GTK writes for every recent item.
static std::string apps(const std::string &mime) {
    return "<info><metadata owner=\"http://freedesktop.org\"><mime:mime-type type=\"" + mime +
           "\"/><bookmark:applications><bookmark:application name=\"gedit\" exec=\"&apos;gedit %u&apos;\" "
           "modified=\"2026-01-01T00:00:00Z\" count=\"1\"/></bookmark:applications></metadata></info>";
}

TEST(recents_read_and_add) {
    const std::string d = private_dir("recents");
    write_file(d + "/old.txt", "o");
    write_file(d + "/new.pdf", "n");
    write_file(d + "/.secret", "s");
    const std::string xbel = d + "/recently-used.xbel";
    write_file(xbel,
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<xbel version=\"1.0\" xmlns:bookmark=\"http://www.freedesktop.org/standards/desktop-bookmarks\" "
               "xmlns:mime=\"http://www.freedesktop.org/standards/shared-mime-info\">\n"
               "<bookmark href=\"" + path_to_uri(d + "/old.txt") + "\" added=\"2026-01-01T00:00:00Z\" "
               "modified=\"2026-01-01T00:00:00Z\" visited=\"2026-01-01T00:00:00Z\">" + apps("text/plain") + "</bookmark>\n"
               "<bookmark href=\"" + path_to_uri(d + "/new.pdf") + "\" added=\"2026-02-01T00:00:00Z\" "
               "modified=\"2026-02-01T00:00:00Z\" visited=\"2026-03-01T00:00:00Z\">" + apps("application/pdf") + "</bookmark>\n"
               "<bookmark href=\"" + path_to_uri(d + "/.secret") + "\" added=\"2026-04-01T00:00:00Z\" "
               "modified=\"2026-04-01T00:00:00Z\" visited=\"2026-04-01T00:00:00Z\">" + apps("text/plain") + "</bookmark>\n"
               "<bookmark href=\"" + path_to_uri(d + "/gone.txt") + "\" added=\"2026-05-01T00:00:00Z\" "
               "modified=\"2026-05-01T00:00:00Z\" visited=\"2026-05-01T00:00:00Z\">" + apps("text/plain") + "</bookmark>\n"
               "<bookmark href=\"https://example.com/\" added=\"2026-05-01T00:00:00Z\" "
               "modified=\"2026-05-01T00:00:00Z\" visited=\"2026-05-01T00:00:00Z\">" + apps("text/html") + "</bookmark>\n"
               "</xbel>\n");
    auto all = recents::read(xbel);
    CHECK_EQ(all.size(), static_cast<std::size_t>(4));
    CHECK(!all.empty() && all[0].path == d + "/gone.txt");
    auto l = recents::list(xbel, false);
    CHECK(l.entries.size() == 2 && l.entries[0].name == d + "/new.pdf" && l.path_of(1) == d + "/old.txt");
    CHECK_EQ(recents::list(xbel, true).entries.size(), static_cast<std::size_t>(3));
    // Opening a file adds it, newest first, and keeps the others.
    CHECK(recents::add(d + "/old.txt", "text/plain", xbel));
    all = recents::read(xbel);
    CHECK(all.size() == 4 && all[0].path == d + "/old.txt");
    const std::string text = read_file(xbel);
    CHECK(text.find("name=\"flo\"") != std::string::npos);
    struct stat st;
    CHECK(::stat(xbel.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    // A corrupt file is never replaced.
    write_file(d + "/bad.xbel", "<xbel><bookmark");
    CHECK(!recents::add(d + "/old.txt", "text/plain", d + "/bad.xbel"));
    CHECK_EQ(read_file(d + "/bad.xbel"), std::string("<xbel><bookmark"));
    CHECK(path_inside(recents::xbel_path(), std::getenv("FLO_TEST_RUN")));
}

TEST(tailscale_status_parsing) {
    const std::string json = R"({
      "Version": "1.80.0",
      "Self": {"HostName": "node", "DNSName": "node.tail1.ts.net.", "OS": "linux", "Online": true},
      "Peer": {
        "nodekey:1": {"HostName": "m2", "DNSName": "m2.tail1.ts.net.", "OS": "macOS", "Online": true,
                      "TailscaleIPs": ["100.64.0.2", "fd7a::2"]},
        "nodekey:2": {"HostName": "phone", "DNSName": "phone.tail1.ts.net.", "OS": "iOS", "Online": true},
        "nodekey:3": {"HostName": "Desk", "DNSName": "desk.tail1.ts.net.", "OS": "linux", "Online": false},
        "nodekey:4": {"HostName": "box", "DNSName": "box.tail1.ts.net.", "OS": "linux", "Online": true},
        "nodekey:5": {"HostName": "win", "DNSName": "win.tail1.ts.net.", "OS": "windows", "Online": true}
      }})";
    auto peers = parse_tailscale_status(json);
    CHECK_EQ(peers.size(), static_cast<std::size_t>(3));
    CHECK(peers.size() == 3 && peers[0].name == "box" && peers[1].name == "m2" && peers[2].name == "win");
    CHECK(peers.size() == 3 && peers[1].host == "m2.tail1.ts.net" && peers[1].ip == "100.64.0.2");
    CHECK_EQ(parse_tailscale_status(json, false).size(), static_cast<std::size_t>(5));
    CHECK(parse_tailscale_status("not json").empty());
    CHECK(parse_tailscale_status("{\"Peer\": null}").empty());
    CHECK_EQ(parse_dropbox_status("Up to date\n"), std::string("synced"));
    CHECK_EQ(parse_dropbox_status("Syncing 3 files\n"), std::string("syncing"));
    CHECK_EQ(parse_dropbox_status("Dropbox isn't running!\n"), std::string("stopped"));
    CHECK_EQ(parse_dropbox_status(""), std::string("stopped"));
    CHECK_EQ(sftp_uri("m2.tail1.ts.net"), std::string("sftp://m2.tail1.ts.net/"));
    CHECK(path_inside(gvfs_sftp_path("m2"), std::getenv("FLO_TEST_RUN")));
}

TEST(physical_drives_skip_the_system) {
    const std::string text =
        "1 0 259:2 / / rw - btrfs /dev/nvme0n1p2 rw\n"
        "2 1 259:2 /@home /home rw - btrfs /dev/nvme0n1p2 rw\n"
        "3 1 259:1 / /boot rw - vfat /dev/nvme0n1p1 rw\n"
        "4 1 8:17 / /run/media/u/USB rw - exfat /dev/sdb1 rw\n"
        "5 1 8:17 / /mnt/backups/again rw - exfat /dev/sdb1 rw\n"
        "6 1 8:33 / /data rw - ext4 /dev/sdc1 rw\n"
        "7 1 7:0 / /snap/x rw - squashfs /dev/loop0 ro\n"
        "8 1 0:40 / /tmp rw - tmpfs tmpfs rw\n";
    auto drives = places::physical_drives(parse_mountinfo(text), "/dev/nvme0n1p2", {{"/dev/sdc1", "Big Data"}});
    CHECK_EQ(drives.size(), static_cast<std::size_t>(2));
    CHECK(drives.size() == 2 && drives[0].path == "/data" && drives[0].name == "Big Data" && !drives[0].removable);
    CHECK(drives.size() == 2 && drives[1].path == "/run/media/u/USB" && drives[1].removable && drives[1].name == "USB");
}

namespace {
bool pump(EventQueue &q, const std::function<bool()> &done, int ms = 5000) {
    for (int waited = 0; waited < ms && !done(); waited += 20) {
        q.wait(20);
        q.dispatch();
    }
    return done();
}
} // namespace

TEST(fake_gvfs_drives_the_mount_operation) {
    const std::string d = private_dir("fake-gvfs");
    ::setenv("FLO_TEST_FAKE_GVFS", d.c_str(), 1);
    EventQueue q;
    Mounter m(q);
    std::vector<MountRequest> asked;
    std::string mounted, failed = "-";
    MountCallbacks cb;
    cb.on_request = [&](const MountRequest &r) { asked.push_back(r); };
    cb.on_mounted = [&](const std::string &local, const std::string &) { mounted = local; };
    cb.on_failed = [&](const std::string &e) { failed = e; };
    // An unknown host: the host-key question reaches the UI, then the password.
    CHECK(m.mount("sftp://box.example/", false, cb));
    CHECK(pump(q, [&] { return asked.size() == 1; }));
    CHECK(asked.size() == 1 && asked[0].kind == MountRequest::Question && asked[0].choices.size() == 2 &&
          asked[0].message.find("box.example") != std::string::npos);
    m.reply_choice(asked[0].id, 0);
    CHECK(pump(q, [&] { return asked.size() == 2; }));
    CHECK(asked.size() == 2 && asked[1].kind == MountRequest::Password && !asked[1].needs_user);
    m.reply_password(asked[1].id, "", "wrong");
    CHECK(pump(q, [&] { return asked.size() == 3; })); // asked again
    m.reply_password(asked[2].id, "", "flo-test");
    CHECK(pump(q, [&] { return !mounted.empty(); }));
    CHECK_EQ(mounted, d + "/sftp:host=box.example");
    // A tailnet machine: the question is answered without asking.
    asked.clear();
    mounted.clear();
    CHECK(m.mount("sftp://laptop.tail1234.ts.net/", true, cb));
    CHECK(pump(q, [&] { return asked.size() == 1; }));
    CHECK(asked.size() == 1 && asked[0].kind == MountRequest::Password);
    m.abort(asked[0].id); // Cancel in the dialog: no error shown
    CHECK(pump(q, [&] { return failed != "-"; }));
    CHECK_EQ(failed, std::string());
    CHECK(mounted.empty());
    ::unsetenv("FLO_TEST_FAKE_GVFS");
}

TEST(mountinfo_fixture_lists_and_ejects_drives) {
    const std::string d = private_dir("mounts");
    const std::string fixture = write_file(d + "/mountinfo",
        "22 1 259:2 / / rw,relatime shared:1 - btrfs /dev/nvme0n1p2 rw\n"
        "40 22 8:17 / /run/media/me/FLO\\040USB rw,nosuid shared:9 - vfat /dev/sdb1 rw\n"
        "41 22 0:5 / /proc rw shared:2 - proc proc rw\n");
    ::setenv("FLO_TEST_MOUNTINFO", fixture.c_str(), 1);
    auto list = places::drives();
    CHECK_EQ(list.size(), std::size_t(1));
    CHECK(list.size() == 1 && list[0].path == "/run/media/me/FLO USB" && list[0].removable && list[0].can_unmount);
    EventQueue q;
    Devices dev(q);
    std::vector<std::vector<Place>> updates;
    dev.watch([&](std::vector<Place> p) { updates.push_back(p); });
    CHECK(pump(q, [&] { return !updates.empty(); }));
    bool done = false, ok = false;
    dev.eject("/run/media/me/FLO USB", [&](bool k, const std::string &, const std::string &) {
        done = true;
        ok = k;
    });
    CHECK(pump(q, [&] { return done; }));
    CHECK(ok);
    CHECK(places::drives().empty());
    CHECK(read_file(fixture).find("FLO\\040USB") == std::string::npos);
    CHECK(read_file(fixture).find("/proc") != std::string::npos); // the rest is kept
    CHECK_EQ(read_file(std::string(std::getenv("FLO_TEST_RUN")) + "/ejects.log"), std::string("eject /run/media/me/FLO USB\n"));
    CHECK(pump(q, [&] { return updates.size() >= 2 && updates.back().empty(); }));
    ::unsetenv("FLO_TEST_MOUNTINFO");
    // Outside a private run the hooks do nothing.
    const char *run = std::getenv("FLO_TEST_RUN");
    std::string keep = run;
    ::setenv("FLO_TEST_MOUNTINFO", fixture.c_str(), 1);
    ::unsetenv("FLO_TEST_RUN");
    CHECK_EQ(xdg::test_hook("FLO_TEST_MOUNTINFO"), std::string());
    ::setenv("FLO_TEST_RUN", keep.c_str(), 1);
    ::unsetenv("FLO_TEST_MOUNTINFO");
}

TEST(filemanager1_uris) {
    auto p = local_paths_from_uris({"file:///a/b%20c", "sftp://h/x", "/plain"});
    CHECK(p.size() == 2 && p[0] == "/a/b c" && p[1] == "/plain");
}

// Tests must never delete trees or touch real user data; product deletion
// lives only in the trash module.
TEST(sources_have_no_tree_deletion_outside_the_trash) {
    const std::string root = FLO_SOURCE_DIR;
    const char *forbidden[] = {"remove_all(", "nftw(", "rm -rf", "removeRecursively", "/.cache/thumbnails\""};
    int hits = 0;
    for (const std::string &dir : {root + "/src/core", root + "/src/thumbnailer", root + "/src", root + "/tests"}) {
        for (const auto &name : list_folder_names(dir)) {
            if (name.size() < 3 || (name.substr(name.size() - 4) != ".cpp" && name.substr(name.size() - 2) != ".h" &&
                                    name.substr(name.size() - 3) != ".sh"))
                continue;
            if (name == "test_places.cpp")
                continue; // this guard names the patterns itself
            const std::string text = read_file(dir + "/" + name);
            for (const char *f : forbidden)
                if (text.find(f) != std::string::npos) {
                    std::printf("     forbidden '%s' in %s/%s\n", f, dir.c_str(), name.c_str());
                    ++hits;
                }
            // Recursive removal (unlinkat with AT_REMOVEDIR) only in trash.cpp.
            if (name != "trash.cpp" && text.find("AT_REMOVEDIR") != std::string::npos) {
                std::printf("     AT_REMOVEDIR in %s\n", name.c_str());
                ++hits;
            }
        }
    }
    CHECK_EQ(hits, 0);
}
