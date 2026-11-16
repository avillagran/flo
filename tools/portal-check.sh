#!/bin/sh
# End-to-end check of Flo as the desktop portal's file chooser, entirely in
# a private session: a fresh 0700 run directory (home, XDG directories and
# fixtures), a private Xvfb, a private dbus-daemon whose only service
# directory is the run's $XDG_DATA_HOME/dbus-1/services, and the system's
# own /usr/lib/xdg-desktop-portal reading the run's user-level
# xdg-desktop-portal configuration. Nothing outside the run directory is
# written and nothing is deleted.
#
#   tools/portal-check.sh   -> prints PASS/FAIL lines and the run directory
#
# Steps: OpenFile through the frontend (org.freedesktop.portal.Desktop),
# picking a file with Enter; SaveFile through the frontend, accepting the
# suggested name; then the backend directly (gdbus on
# org.freedesktop.impl.portal.desktop.flo) for OpenFile with directory=true.
set -u
here=$(cd "$(dirname "$0")/.." && pwd -P)
build=${FLO_BUILD:-$here/build}
X="python3 $here/tools/xdrive.py"
umask 077
mkdir -p "$build/manual-runs"
run=$(mktemp -d "$build/manual-runs/portal-$(date +%Y%m%d-%H%M%S)-XXXXXX")
h=$run/home
mkdir -p "$h/Documents" "$h/.config/xdg-desktop-portal" "$h/.local/share/xdg-desktop-portal/portals" \
  "$h/.local/share/dbus-1/services" "$h/.cache" "$h/.local/state" "$run/runtime" "$run/tmp"
printf 'Day 1: Alfama\n' > "$h/Documents/itinerary.txt"
d=${PORTAL_DISPLAY:-76}
cat > "$run/env.sh" <<ENV
export HOME="$h" XDG_CONFIG_HOME="$h/.config" XDG_DATA_HOME="$h/.local/share" XDG_CACHE_HOME="$h/.cache"
export XDG_STATE_HOME="$h/.local/state" XDG_RUNTIME_DIR="$run/runtime" TMPDIR="$run/tmp"
export XDG_CURRENT_DESKTOP=Hyprland DISPLAY=":$d" FLO_TEST_RUN="$run" FLO_PORTAL_IDLE=8
export GIO_USE_VFS=local GVFS_DISABLE_FUSE=1 FLO_NO_VOLUME_MONITOR=1
unset WAYLAND_DISPLAY HYPRLAND_INSTANCE_SIGNATURE OMARCHY_PATH
ENV
. "$run/env.sh"
# The user-level registration Preferences writes (core/default_apps.cpp).
printf '[portal]\nDBusName=org.freedesktop.impl.portal.desktop.flo\nInterfaces=org.freedesktop.impl.portal.FileChooser;\n' \
  > "$XDG_DATA_HOME/xdg-desktop-portal/portals/flo.portal"
printf '[D-BUS Service]\nName=org.freedesktop.impl.portal.desktop.flo\nExec=%s --portal\n' "$build/flo" \
  > "$XDG_DATA_HOME/dbus-1/services/org.freedesktop.impl.portal.desktop.flo.service"
printf '[preferred]\ndefault=gtk\norg.freedesktop.impl.portal.FileChooser=flo\n' \
  > "$XDG_CONFIG_HOME/xdg-desktop-portal/hyprland-portals.conf"
cat > "$run/dbus.conf" <<CONF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:path=$run/runtime/bus</listen><auth>EXTERNAL</auth>
<servicedir>$XDG_DATA_HOME/dbus-1/services</servicedir>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/></policy>
</busconfig>
CONF
# Xvfb compiles its keymap into /tmp: a private /tmp keeps it off the
# host's (only the X socket directory is shared).
bwrap --dev-bind / / --tmpfs /tmp --bind /tmp/.X11-unix /tmp/.X11-unix --die-with-parent \
  Xvfb ":$d" -screen 0 1440x900x24 -nolisten tcp > "$run/xvfb.log" 2>&1 &
xvfb=$!
dbus-daemon --config-file="$run/dbus.conf" --nofork --nopidfile > "$run/dbus.log" 2>&1 &
bus=$!
export DBUS_SESSION_BUS_ADDRESS="unix:path=$run/runtime/bus"
sleep 0.6
/usr/lib/xdg-desktop-portal -v -r > "$run/portal.log" 2>&1 &
xdp=$!
sleep 1.5
fail=0
check() { if grep -q "$2" "$3"; then echo "PASS $1"; else echo "FAIL $1 (see $3)"; fail=1; fi; }

# 1. OpenFile through the frontend: Enter on the first file picks it.
#    The client stays on the bus until the answer, as applications do.
python3 -I "$here/tools/portal-client.py" open "Pick a file" "$h/Documents" > "$run/open.out" 2>&1 &
sleep 5
$X shot "$run/open-picker.png" key Home sleep 0.3 key Return sleep 1.5
check "OpenFile picks a file through xdg-desktop-portal" "uri file://$h/Documents/itinerary.txt" "$run/open.out"

# 2. SaveFile through the frontend: the suggested name, accepted with Enter.
python3 -I "$here/tools/portal-client.py" save "Save notes" "$h/Documents" notes.txt > "$run/save.out" 2>&1 &
sleep 4
$X shot "$run/save-picker.png" key Return sleep 1.5
check "SaveFile answers the typed name" "uri file://$h/Documents/notes.txt" "$run/save.out"

# 3. The backend directly: folders, answered with the accept button (the
#    picker is 1100x720 at the screen's top left without a window manager;
#    the bar's buttons sit at y 660).
( gdbus call --session --dest org.freedesktop.impl.portal.desktop.flo --object-path /org/freedesktop/portal/desktop \
  --method org.freedesktop.impl.portal.FileChooser.OpenFile /org/freedesktop/portal/desktop/request/1_1/t1 \
  "" "" "Pick a folder" "{'directory': <true>, 'current_folder': <b'$h/Documents'>}" > "$run/folder.out" 2>&1 ) &
sleep 4
$X shot "$run/folder-picker.png" key ctrl+shift+d sleep 0.3 click 1035 660 sleep 1.5
check "OpenFile with directory=true answers the folder (backend)" "file://$h/Documents'" "$run/folder.out"

# 4. Cancel: closing with the Cancel button answers 1.
( gdbus call --session --dest org.freedesktop.impl.portal.desktop.flo --object-path /org/freedesktop/portal/desktop \
  --method org.freedesktop.impl.portal.FileChooser.OpenFile /org/freedesktop/portal/desktop/request/1_1/t2 \
  "" "" "Cancel me" "{}" > "$run/cancel.out" 2>&1 ) &
sleep 4
$X click 925 660 sleep 1.5
check "Cancel answers response 1" "uint32 1" "$run/cancel.out"

kill $xdp 2>/dev/null
sleep 0.3
kill $bus $xvfb 2>/dev/null
wait 2>/dev/null
echo "run: $run"
exit $fail
