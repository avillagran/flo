# Flo as the file chooser

Other programs can use Flo to open and save files. Flo implements the
xdg-desktop-portal backend interface `org.freedesktop.impl.portal.FileChooser`
(`OpenFile`, `SaveFile`, `SaveFiles`), so any application that asks the
desktop portal for a file (Firefox, Chromium, Flatpak apps, GTK and Qt apps
with portals on) gets a Flo window instead of the GTK dialog.

Files did not have this (Nautilus parity row 32: "no"); it is new in Flo.

## What the picker looks like

A picker is a normal Flo window (header, tabs, rail, views, inspector, the
status bar) with one bar at the bottom:

- the filter menu, when the application sent filters (`filters`,
  `current_filter`; globs and MIME types such as `image/*`); files the
  filter does not accept leave the listing, folders always stay;
- the name field when saving (`current_name`, or the name of `current_file`),
  with the stem selected as Files' rename selects it;
- Cancel and the accept button (`accept_label` without its GTK mnemonic, or
  Open / Save / Select).

Opening a file in the browser (Enter, a double click) picks it; folders open
as usual. With `multiple` the marked files are picked; with `directory` the
marked folders, or the folder shown when none is marked. Saving over an
existing file asks first. `SaveFiles` saves every name of `files` in the
folder shown (or the one marked folder). The picker starts in
`current_folder` (home otherwise). Closing the window answers "cancelled";
the portal's `Request.Close` closes it. The portal's extra `choices` are
answered with their defaults (Flo shows none of them). `parent_window` is
not used yet: the picker is a normal top-level window.

## How it is started

`flo --portal` owns the bus name `org.freedesktop.impl.portal.desktop.flo`
and serves `/org/freedesktop/portal/desktop`. It is a separate process from
the file manager windows; D-Bus starts it when xdg-desktop-portal first asks,
and it leaves after a minute without a picker (`FLO_PORTAL_IDLE` seconds in
tests). It does not own Flo's single-instance name or FileManager1 and does
not save the session.

Two files register it:

- `flo.portal` (`packaging/flo.portal`) tells xdg-desktop-portal the backend
  exists: `DBusName=org.freedesktop.impl.portal.desktop.flo`,
  `Interfaces=org.freedesktop.impl.portal.FileChooser;`.
- `org.freedesktop.impl.portal.desktop.flo.service`
  (`packaging/org.freedesktop.impl.portal.desktop.flo.service`) lets D-Bus
  start `flo --portal`.

The package installs them under `/usr/share/xdg-desktop-portal/portals/` and
`/usr/share/dbus-1/services/`. Without the package, Preferences → Default
applications → "Use Flo as the file chooser" writes user-level copies under
`$XDG_DATA_HOME` (`~/.local/share/xdg-desktop-portal/portals/flo.portal` and
`~/.local/share/dbus-1/services/…service`, `Exec=` pointing at the running
Flo binary). Being registered changes nothing by itself: a portal
configuration has to name Flo.

## Choosing Flo (user level, no root)

Checked on this system with xdg-desktop-portal 1.22.1 (`xdg-desktop-portal
-v` in a private session, and end to end with `tools/portal-check.sh`):

- Backends are loaded from `$XDG_DATA_HOME/xdg-desktop-portal/portals` as
  well as the system data directories, so a user-level `flo.portal` works.
- The session bus starts services from `$XDG_DATA_HOME/dbus-1/services`.
- The configuration is looked up as `<desktop>-portals.conf` (for each
  desktop in `$XDG_CURRENT_DESKTOP`, lowercased: `hyprland`) and then
  `portals.conf`, in `~/.config/xdg-desktop-portal`, `$XDG_CONFIG_DIRS`,
  `/etc/xdg-desktop-portal`, `$XDG_DATA_HOME` and `$XDG_DATA_DIRS`
  (`…/xdg-desktop-portal`). An interface line in the user's `portals.conf`
  beats a system `hyprland-portals.conf`, but a user `hyprland-portals.conf`
  hides the user's `portals.conf`.

Omarchy on Hyprland ships `/usr/share/xdg-desktop-portal/hyprland-portals.conf`
(`default=hyprland;gtk`); xdg-desktop-portal-hyprland has no file chooser, so
files are picked by xdg-desktop-portal-gtk. On this machine the user already
has `~/.config/xdg-desktop-portal/hyprland-portals.conf`
(`default=hyprland;gtk`, the Secret portal on gnome-keyring), so that is the
file to edit.

"Use Flo as the file chooser" (Preferences → Default applications) does
exactly this, after a confirmation: registers the backend for the user when
it is not installed system-wide, and adds

    org.freedesktop.impl.portal.FileChooser=flo

to the `[preferred]` group of the user's `<desktop>-portals.conf` when it
exists, otherwise of `~/.config/xdg-desktop-portal/portals.conf` (a new one
starts as a copy of the system file, so nothing else changes). Other lines
stay. The previous value is kept in Flo's settings (`[defaults]
previous-file-chooser`) and "Restore the previous file chooser" puts it back
(or removes the line when there was none).

The change takes effect when xdg-desktop-portal starts again:

    systemctl --user restart xdg-desktop-portal.service

or a new login. Nothing here needs root.

By hand, the same is:

    mkdir -p ~/.local/share/xdg-desktop-portal/portals ~/.local/share/dbus-1/services
    cp packaging/flo.portal ~/.local/share/xdg-desktop-portal/portals/
    sed "s|/usr/bin/flo|$HOME/.local/bin/flo|" \
      packaging/org.freedesktop.impl.portal.desktop.flo.service \
      > ~/.local/share/dbus-1/services/org.freedesktop.impl.portal.desktop.flo.service
    # then add the FileChooser=flo line to the [preferred] group of
    # ~/.config/xdg-desktop-portal/hyprland-portals.conf (or portals.conf)
    systemctl --user restart xdg-desktop-portal.service

## Checking it

`tools/portal-check.sh` runs the whole path in a private session: a 0700 run
directory with its own home and XDG directories, a private Xvfb, a private
dbus-daemon whose only service directory is the run's
`$XDG_DATA_HOME/dbus-1/services`, and the system's
`/usr/lib/xdg-desktop-portal` reading the run's user-level configuration.
It checks `OpenFile` and `SaveFile` through the frontend
(`tools/portal-client.py` stays on the bus until the answer, as applications
do: the portal closes the requests of callers that leave), then `OpenFile`
with `directory` and a cancel on the backend directly. The core tests cover
the option parsing, the results and the choice rules (`tests/test_portal.cpp`)
and the configuration edits (`tests/test_default_apps.cpp`), on private XDG
directories only.

`FLO_TRACE_PORTAL=1` prints each request the backend receives.
