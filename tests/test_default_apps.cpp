// Flo as the default file manager and file chooser (core/default_apps.h):
// only the one line changes, and restoring puts back exactly what was
// there. Everything happens under the run's private XDG directories.
#include "harness.h"

#include "core/default_apps.h"
#include "core/fsutil.h"

#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

using namespace flo;
using namespace flo::default_apps;
using namespace flo_test;

namespace {

// Points every XDG variable this module reads at a fresh fixture, and puts
// the run's own values back when the test ends (other tests read the MIME
// database through XDG_DATA_DIRS).
struct EnvScope {
    std::vector<std::pair<std::string, std::string>> saved;
    std::vector<std::string> unset_after;
    EnvScope() {
        for (const char *k : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CONFIG_DIRS", "XDG_DATA_DIRS",
                              "XDG_CURRENT_DESKTOP"}) {
            if (const char *v = std::getenv(k))
                saved.emplace_back(k, v);
            else
                unset_after.push_back(k);
        }
    }
    ~EnvScope() {
        for (const auto &kv : saved)
            setenv(kv.first.c_str(), kv.second.c_str(), 1);
        for (const auto &k : unset_after)
            unsetenv(k.c_str());
    }
};

std::string private_xdg(const std::string &label) {
    std::string d = private_dir(label);
    for (const char *s : {"/config", "/data", "/sysconf", "/sysdata"})
        make_dirs(d + s, 0700);
    setenv("XDG_CONFIG_HOME", (d + "/config").c_str(), 1);
    setenv("XDG_DATA_HOME", (d + "/data").c_str(), 1);
    setenv("XDG_CONFIG_DIRS", (d + "/sysconf").c_str(), 1);
    setenv("XDG_DATA_DIRS", (d + "/sysdata").c_str(), 1);
    setenv("XDG_CURRENT_DESKTOP", "Hyprland", 1);
    return d;
}

} // namespace

TEST(default_apps_ini_edits_touch_one_line) {
    const std::string text = "# keep\n[A]\nx=1\n\n[B]\ny=2\n";
    CHECK_EQ(ini_get(text, "A", "x"), std::string("1"));
    bool found = true;
    ini_get(text, "A", "y", &found);
    CHECK(!found);
    CHECK_EQ(ini_set(text, "A", "x", "9"), std::string("# keep\n[A]\nx=9\n\n[B]\ny=2\n"));
    CHECK_EQ(ini_set(text, "A", "z", "3"), std::string("# keep\n[A]\nx=1\nz=3\n\n[B]\ny=2\n"));
    CHECK_EQ(ini_set(text, "C", "k", "v"), std::string("# keep\n[A]\nx=1\n\n[B]\ny=2\n\n[C]\nk=v\n"));
    CHECK_EQ(ini_remove(text, "B", "y"), std::string("# keep\n[A]\nx=1\n\n[B]\n"));
    CHECK_EQ(ini_set("", "G", "k", "v"), std::string("[G]\nk=v\n"));
    Previous p;
    p.had = true, p.value = "a;b%c", p.had2 = false, p.valid = true;
    Previous q = Previous::decode(p.encode());
    CHECK(q.valid && q.had && !q.had2);
    CHECK_EQ(q.value, std::string("a;b%c"));
    CHECK(!Previous::decode("junk").valid);
}

TEST(default_apps_file_manager_changes_only_inode_directory_and_restores) {
    EnvScope env;
    std::string d = private_xdg("defaults-fm");
    const std::string file = mimeapps_path();
    CHECK_EQ(file, d + "/config/mimeapps.list");
    const std::string before = "[Default Applications]\ntext/plain=nvim.desktop\ninode/directory=org.gnome.Nautilus.desktop\n"
                               "\n[Added Associations]\ninode/directory=org.gnome.Nautilus.desktop;thunar.desktop;\n"
                               "image/png=imv.desktop;\n";
    write_file(file, before);
    CHECK_EQ(user_default(file, kDirectoryMime), std::string("org.gnome.Nautilus.desktop"));
    Previous prev;
    CHECK_EQ(set_default(file, kDirectoryMime, kFloDesktopId, &prev), std::string());
    std::string now = read_file(file);
    CHECK_EQ(now, std::string("[Default Applications]\ntext/plain=nvim.desktop\ninode/directory=flo.desktop\n"
                              "\n[Added Associations]\ninode/directory=flo.desktop;org.gnome.Nautilus.desktop;thunar.desktop;\n"
                              "image/png=imv.desktop;\n"));
    CHECK(prev.valid && prev.had && prev.had2);
    CHECK_EQ(user_default(file, kDirectoryMime), std::string("flo.desktop"));
    CHECK_EQ(restore_default(file, kDirectoryMime, Previous::decode(prev.encode())), std::string());
    CHECK_EQ(read_file(file), before);
    // Bad ids are refused and change nothing.
    CHECK(!set_default(file, kDirectoryMime, "flo", nullptr).empty());
    CHECK(!set_default(file, "inode/dir\nx", kFloDesktopId, nullptr).empty());
    CHECK_EQ(read_file(file), before);
}

TEST(default_apps_file_manager_from_nothing_restores_to_nothing) {
    EnvScope env;
    std::string d = private_xdg("defaults-fm-new");
    const std::string file = mimeapps_path();
    Previous prev;
    CHECK_EQ(set_default(file, kDirectoryMime, kFloDesktopId, &prev), std::string());
    CHECK_EQ(read_file(file), std::string("[Default Applications]\ninode/directory=flo.desktop\n\n"
                                          "[Added Associations]\ninode/directory=flo.desktop;\n"));
    CHECK(prev.valid && !prev.had && !prev.had2);
    CHECK_EQ(restore_default(file, kDirectoryMime, prev), std::string());
    CHECK_EQ(read_file(file), std::string("[Default Applications]\n\n[Added Associations]\n"));
}

TEST(default_apps_mimeapps_link_keeps_pointing_at_its_file) {
    EnvScope env;
    std::string d = private_xdg("defaults-link");
    // A dotfiles-style link inside the fixture: the target is rewritten,
    // the link stays a link.
    std::string real = write_file(d + "/dotfiles-mimeapps.list", "[Default Applications]\n");
    CHECK(::symlink(real.c_str(), mimeapps_path().c_str()) == 0);
    CHECK_EQ(set_default(mimeapps_path(), kDirectoryMime, kFloDesktopId), std::string());
    struct stat st;
    CHECK(::lstat(mimeapps_path().c_str(), &st) == 0 && S_ISLNK(st.st_mode));
    CHECK_EQ(user_default(real, kDirectoryMime), std::string("flo.desktop"));
}

TEST(default_apps_file_chooser_edits_the_user_desktop_file) {
    EnvScope env;
    std::string d = private_xdg("defaults-portal");
    // The machine's case: a user hyprland-portals.conf exists (it hides the
    // user's portals.conf), and gtk implements FileChooser.
    make_dirs(d + "/config/xdg-desktop-portal", 0700);
    make_dirs(d + "/sysdata/xdg-desktop-portal/portals", 0700);
    write_file(d + "/sysdata/xdg-desktop-portal/portals/gtk.portal",
               "[portal]\nDBusName=org.freedesktop.impl.portal.desktop.gtk\nInterfaces=org.freedesktop.impl.portal.FileChooser;org.freedesktop.impl.portal.Print;\n");
    write_file(d + "/sysdata/xdg-desktop-portal/portals/hyprland.portal",
               "[portal]\nDBusName=org.freedesktop.impl.portal.desktop.hyprland\nInterfaces=org.freedesktop.impl.portal.Screenshot;\n");
    const std::string user = d + "/config/xdg-desktop-portal/hyprland-portals.conf";
    const std::string before = "[preferred]\ndefault=hyprland;gtk\norg.freedesktop.impl.portal.Secret=gnome-keyring\n";
    write_file(user, before);
    auto desk = current_desktops();
    CHECK(desk == std::vector<std::string>{"hyprland"});
    CHECK_EQ(portal_config_path(desk), user);
    CHECK_EQ(file_chooser_backend(desk), std::string("gtk"));
    Previous prev;
    CHECK_EQ(set_file_chooser(desk, kPortalName, &prev), std::string());
    CHECK_EQ(read_file(user), before + "org.freedesktop.impl.portal.FileChooser=flo\n");
    CHECK_EQ(file_chooser_backend(desk), std::string("flo"));
    CHECK_EQ(restore_file_chooser(desk, prev), std::string());
    CHECK_EQ(read_file(user), before);
}

TEST(default_apps_file_chooser_seeds_a_new_portals_conf) {
    EnvScope env;
    std::string d = private_xdg("defaults-portal-new");
    make_dirs(d + "/sysdata/xdg-desktop-portal", 0700);
    const std::string sys = "[preferred]\ndefault=hyprland;gtk\n";
    write_file(d + "/sysdata/xdg-desktop-portal/hyprland-portals.conf", sys);
    auto desk = current_desktops();
    CHECK_EQ(effective_portal_config(desk), d + "/sysdata/xdg-desktop-portal/hyprland-portals.conf");
    std::string seed;
    CHECK_EQ(portal_config_path(desk, &seed), d + "/config/xdg-desktop-portal/portals.conf");
    CHECK_EQ(seed, d + "/sysdata/xdg-desktop-portal/hyprland-portals.conf");
    Previous prev;
    CHECK_EQ(set_file_chooser(desk, kPortalName, &prev), std::string());
    CHECK_EQ(read_file(d + "/config/xdg-desktop-portal/portals.conf"),
             sys + "org.freedesktop.impl.portal.FileChooser=flo\n");
    CHECK(!prev.had);
    CHECK_EQ(read_file(d + "/sysdata/xdg-desktop-portal/hyprland-portals.conf"), sys);
    CHECK_EQ(restore_file_chooser(desk, prev), std::string());
    CHECK_EQ(read_file(d + "/config/xdg-desktop-portal/portals.conf"), sys);
}

TEST(default_apps_backend_registration_is_user_level) {
    EnvScope env;
    std::string d = private_xdg("defaults-register");
    CHECK(installed_portal_file().empty());
    CHECK_EQ(install_portal_backend("/opt/flo/bin/flo"), std::string());
    CHECK_EQ(installed_portal_file(), d + "/data/xdg-desktop-portal/portals/flo.portal");
    CHECK_EQ(read_file(installed_portal_file()), portal_file_text());
    CHECK_EQ(installed_dbus_service(), d + "/data/dbus-1/services/org.freedesktop.impl.portal.desktop.flo.service");
    CHECK(read_file(installed_dbus_service()).find("Exec=/opt/flo/bin/flo --portal\n") != std::string::npos);
    CHECK(!install_portal_backend("relative/flo").empty());
}
