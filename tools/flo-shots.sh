#!/bin/sh
# Flo's own screenshots beyond the side-by-side set (tools/side-by-side.sh):
# the drives and network rail, the SFTP prompt, rename, the conflict
# prompt, the trash with its undo toast, an empty folder, the shortcuts
# and the preview. One private Xvfb session with tools/session-fixtures.sh
# (fake tailscale, a removable drive, a fake GVfs); every file operation
# happens inside that run's private home. Nothing is deleted outside it.
#
#   tools/flo-shots.sh COLORS_TOML
set -eu
here=$(cd "$(dirname "$0")/.." && pwd -P)
out=$here/docs/screenshots
X="python3 $here/tools/xdrive.py"
d=73
r=$(FLO_SESSION_THEME=$1 FLO_SESSION_SETUP=$here/tools/session-fixtures.sh sh "$here/tools/gui-session.sh" start $d 2>/dev/null | tail -1)
export DISPLAY=":$d"
shot() { $X sleep "$2" shot "$r/raw-$1.png"; magick "$r/raw-$1.png" -crop 1100x700+0+20 +repage "$out/$1.png"; }
sleep 3
$X move 26 300; shot drives-network 1.6
$X move 600 400 sleep 0.3 key ctrl+l sleep 0.3 type ":studio" key Return sleep 0.6 key Return; shot sftp-prompt 2.5
$X key Escape sleep 0.5 key ctrl+l sleep 0.3 key ctrl+a type "$r/home/Projects" key Return; shot empty 1.5
$X key ctrl+l sleep 0.3 key ctrl+a type "$r/home/Documents" key Return sleep 1.2 key Home key ctrl+c
$X key ctrl+l sleep 0.3 key ctrl+a type "$r/home/Pictures/Lisbon 2026" key Return sleep 2 key ctrl+v sleep 1.5 key ctrl+v; shot conflict 1.5
$X key Escape sleep 0.5 key ctrl+2 sleep 0.5 key End key F2; shot rename 1.0
$X key Escape sleep 0.5 key Delete; shot undo-toast 0.8
$X sleep 3 key ctrl+l sleep 0.3 type ":trash" key Return sleep 0.6 key Return; shot trash 1.5
$X key F1; shot help 0.8
$X key Escape sleep 0.4 key alt+Left sleep 1.5 key Home key space; $X sleep 3 shot "$r/raw-preview.png"
cp "$r/raw-preview.png" "$out/preview-screen.png"
sh "$here/tools/gui-session.sh" stop "$r"
echo "$r"
