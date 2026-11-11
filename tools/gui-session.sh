#!/bin/sh
# Starts Flo in a private Xvfb with private fixture data, for screenshots and
# manual checks. Nothing here deletes anything; every run is kept.
#
#   tools/gui-session.sh start [DISPLAY_NUMBER] [FOLDER]   -> prints the run dir
#   tools/gui-session.sh stop RUN_DIR                     -> stops what start began
#
# The run directory (build/manual-runs/run-*) is 0700 and holds home/ with
# the fixtures (Documents, Pictures/Lisbon 2026 with 50 photos, ...), tmp/,
# the XDG directories, env.sh, sandbox.sh and the logs. Flo runs inside
# bubblewrap: the host is read-only except the run directory, there is no
# session bus, and only the X socket is shared. Drive it with tools/xdrive.py.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd -P)
build=${FLO_BUILD:-$here/build}

case ${1:-} in
start)
  disp=${2:-37}
  umask 077
  mkdir -p "$build/manual-runs"
  run=$(mktemp -d "$build/manual-runs/run-$(date +%Y%m%d-%H%M%S)-XXXXXX")
  chmod 0700 "$run"
  for d in tmp home home/.config home/.config/gtk-3.0 home/.local home/.local/share home/.cache \
           home/.local/state runtime; do
    mkdir -p "$run/$d"
  done
  h=$run/home
  for d in Documents Downloads Music Pictures Projects Videos Work; do mkdir -p "$h/$d"; done
  printf 'Flo fixture\n' > "$h/Documents/readme.txt"
  printf 'Day 1: Alfama\nDay 2: Sintra\n' > "$h/Documents/itinerary.txt"
  lis="$h/Pictures/Lisbon 2026"
  mkdir -p "$lis/Alfama" "$lis/Belém" "$lis/Sintra day trip"
  printf '# Trip notes\n' > "$lis/trip-notes.md"
  colors="tomato sienna slategray midnightblue mediumaquamarine seagreen khaki burlywood cadetblue darkslategray"
  i=1
  while [ $i -le 50 ]; do
    c=$(echo $colors | cut -d' ' -f$(( (i - 1) % 10 + 1 )))
    magick -size 1600x1200 "radial-gradient:white-$c" -quality 70 "$lis/$(printf 'IMG_%04d.jpg' $i)"
    i=$((i + 1))
  done
  magick -size 800x600 gradient:steelblue-darkgreen "$lis/Sintra day trip/forest-walk.jpg"
  magick -size 800x600 gradient:wheat-slategray "$lis/Sintra day trip/pena-palace.jpg"
  magick -size 640x480 gradient:gold-firebrick "$lis/Alfama/tram-28.png"
  printf 'file://%s Lisbon\n' "$(printf '%s' "$lis" | sed 's/ /%20/g')" > "$h/.config/gtk-3.0/bookmarks"

  cat > "$run/env.sh" <<EOF
export TMPDIR="$run/tmp" HOME="$h" XDG_CONFIG_HOME="$h/.config" XDG_DATA_HOME="$h/.local/share"
export XDG_CACHE_HOME="$h/.cache" XDG_STATE_HOME="$h/.local/state" XDG_RUNTIME_DIR="$run/runtime"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$run/runtime/no-bus" GIO_USE_VFS=local GVFS_DISABLE_FUSE=1
export DISPLAY=":$disp" FLO_TEST_RUN="$run"
unset WAYLAND_DISPLAY HYPRLAND_INSTANCE_SIGNATURE OMARCHY_PATH
EOF
  cat > "$run/sandbox.sh" <<EOF
#!/bin/sh
# Runs a command in the sandbox used for this run.
. "$run/env.sh"
exec bwrap --unshare-pid --die-with-parent \\
  --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib /lib64 --symlink usr/bin /bin --symlink usr/bin /sbin \\
  --ro-bind /etc /var/etc --symlink var/etc /etc --proc /proc --dev /dev --tmpfs /tmp --bind /tmp/.X11-unix /tmp/.X11-unix \\
  --ro-bind "$build" "$build" --bind "$run" "$run" --chdir "$h" "\$@"
EOF
  chmod 0700 "$run/sandbox.sh"
  Xvfb ":$disp" -screen 0 1440x900x24 -nolisten tcp > "$run/xvfb.log" 2>&1 &
  echo $! > "$run/xvfb.pid"
  sleep 1
  # bwrap dies with its parent: a waiting shell keeps it alive past this
  # script, and stopping that shell stops the sandbox.
  sh -c '"$1/sandbox.sh" "$2" "$3"; true' sh "$run" "$build/flo" "${3:-$h}" > "$run/flo.log" 2>&1 &
  echo $! > "$run/flo.pid"
  sleep 1.5
  echo "$run"
  ;;
stop)
  run=$2
  for f in flo xvfb; do
    [ -f "$run/$f.pid" ] && kill "$(cat "$run/$f.pid")" 2>/dev/null || true
  done
  ;;
*)
  echo "usage: $0 start [DISPLAY] [FOLDER] | stop RUN_DIR" >&2
  exit 2
  ;;
esac
