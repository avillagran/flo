#!/bin/sh
# Side-by-side screenshots of Files (the Rust app Flo replaces) and Flo in
# the same states, each in its own private Xvfb session with the same
# fixture data and the same Omarchy palette (tools/gui-session.sh). The
# pairs land in docs/screenshots/files-vs-flo/, Flo's own shots in
# docs/screenshots/. Nothing is deleted; every run stays in build/.
#
#   tools/side-by-side.sh FILES_BINARY COLORS_TOML
set -eu
here=$(cd "$(dirname "$0")/.." && pwd -P)
files_bin=$1
theme=$2
out=$here/docs/screenshots
mkdir -p "$out/files-vs-flo"
X="python3 $here/tools/xdrive.py"

start() { # app display -> run dir
  FLO_SESSION_APP=$1 FLO_SESSION_THEME=$theme sh "$here/tools/gui-session.sh" start "$2" 2>/dev/null | tail -1
}
files_run=$(start "$files_bin" 71)
flo_run=$(start "$here/build/flo" 72)
sleep 2

# Both apps take the same keys. $1: display, $2: run dir.
drive() {
  d=$1
  r=$2
  lis="$r/home/Pictures/Lisbon 2026"
  shot() { DISPLAY=":$d" $X sleep "$2" shot "$r/raw-$1.png"; magick "$r/raw-$1.png" -crop 1100x700+0+20 +repage "$r/$1.png"; }
  DISPLAY=":$d" $X move 700 650 key ctrl+l sleep 0.4 key ctrl+a type "$lis" key Return sleep 3
  shot groups 0.5
  DISPLAY=":$d" $X key ctrl+2; shot list 1.5
  DISPLAY=":$d" $X key ctrl+5; shot grid 2.5
  DISPLAY=":$d" $X key ctrl+3; shot columns 1.5
  DISPLAY=":$d" $X key ctrl+4; shot space 2.5
  DISPLAY=":$d" $X key ctrl+1 sleep 0.5 key F9; shot dual 1.5
  DISPLAY=":$d" $X key F9 sleep 0.5 key F10; shot actions 0.8
  DISPLAY=":$d" $X key Escape sleep 0.3 key ctrl+comma; shot preferences 0.8
  DISPLAY=":$d" $X key Escape sleep 0.3 key ctrl+shift+p; shot commands 0.8
  DISPLAY=":$d" $X key Escape sleep 0.3 key ctrl+l; shot omnibar 0.6
  DISPLAY=":$d" $X key Escape sleep 0.3 type img_001; shot filter 1.0
  DISPLAY=":$d" $X key Escape sleep 0.3 move 26 300; shot rail 1.4
  DISPLAY=":$d" $X move 600 500 sleep 0.3 button 3 600 470; shot menu 0.8
  DISPLAY=":$d" $X key Escape sleep 0.3 move 1028 48; shot hover 2.6
}
drive 71 "$files_run" &
drive 72 "$flo_run"
wait
for s in groups list grid columns space dual actions preferences commands omnibar filter rail menu hover; do
  magick "$files_run/$s.png" "$flo_run/$s.png" +append -resize 2208x "$out/files-vs-flo/$s.png"
  cp "$flo_run/$s.png" "$out/$s.png"
done
sh "$here/tools/gui-session.sh" stop "$files_run"
sh "$here/tools/gui-session.sh" stop "$flo_run"
echo "Files: $files_run"
echo "Flo:   $flo_run"
