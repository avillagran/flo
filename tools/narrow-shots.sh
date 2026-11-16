#!/bin/sh
# The adaptive header and status bar at three window widths (420, 800 and
# 1900 px), with the overflow column open at 420 px. One private Xvfb
# session (tools/gui-session.sh) with its fixtures; nothing outside the
# run directory is touched and nothing is deleted.
#
#   tools/narrow-shots.sh [COLORS_TOML] [OUT_DIR]
set -eu
here=$(cd "$(dirname "$0")/.." && pwd -P)
out=${2:-$here/docs/screenshots/narrow}
mkdir -p "$out"
X="python3 $here/tools/xdrive.py"
d=74
r=$(FLO_SESSION_THEME=${1:-} FLO_SESSION_SCREEN=2000x1000x24 sh "$here/tools/gui-session.sh" start $d \
    2>/dev/null | tail -1)
export DISPLAY=":$d"
sleep 3
lis="$r/home/Pictures/Lisbon 2026/Sintra day trip"
$X key ctrl+l sleep 0.3 key ctrl+a type "$lis" key Return sleep 1.5 key Home key Insert
for w in 1900 800 420; do
  $X resize $w 640 sleep 1.2 move 1990 990 sleep 0.4 shot "$r/raw-$w.png"
  magick "$r/raw-$w.png" -crop ${w}x640+0+20 +repage "$out/width-$w.png"
done
# Hovering the overflow button drops the folded buttons down.
ox=$(( 420 - 12 - 36 - 4 - 18 ))
$X move $ox 48 sleep 0.8 shot "$r/raw-420-open.png"
magick "$r/raw-420-open.png" -crop 420x640+0+20 +repage "$out/width-420-overflow.png"
sh "$here/tools/gui-session.sh" stop "$r"
echo "$r"
