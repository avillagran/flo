#!/bin/sh
# Screenshots of this round's features in one private Xvfb session
# (tools/gui-session.sh): the Connect dialog with fixture servers (the
# FLO_TEST_AVAHI and FLO_TEST_GIO_NETWORK hooks stand in for the network)
# and recent ones, Preferences > Default applications, the folder cards
# with the sigma mark, and the List, Grid and Columns views with their
# icons and text previews. Nothing outside the run directory is touched.
#
#   tools/feature-shots.sh [COLORS_TOML] [OUT_DIR]
set -eu
here=$(cd "$(dirname "$0")/.." && pwd -P)
out=${2:-$here/docs/screenshots}
mkdir -p "$out"
X="python3 $here/tools/xdrive.py"
d=75
setup=$(mktemp "$here/build/manual-runs/feature-setup-XXXXXX.sh")
cat > "$setup" <<'SETUP'
#!/bin/sh
run=$1
h=$run/home
cat > "$run/avahi.txt" <<'AV'
=;eth0;IPv4;nas;_smb._tcp;local;nas.local;192.168.1.10;445;
=;eth0;IPv4;studio;_sftp-ssh._tcp;local;studio.local;192.168.1.20;22;
=;eth0;IPv4;Photos\032on\032nas;_webdav._tcp;local;nas.local;192.168.1.10;8080;
=;eth0;IPv4;printer-share;_nfs._tcp;local;archive.local;192.168.1.30;2049;
AV
cat > "$run/gio.txt" <<'GIO'
fotos	0	(shortcut)	standard::display-name=fotos on nas	standard::target-uri=smb://nas.local/fotos/
GIO
echo "export FLO_TEST_AVAHI=$run/avahi.txt FLO_TEST_GIO_NETWORK=$run/gio.txt" >> "$run/env.sh"
mkdir -p "$h/.config/flo"
printf '[network]\nrecent=sftp://studio.local/ smb://nas.local/fotos/\n' > "$h/.config/flo/flo.conf"
# Text and code files for the previews, and a few folders to measure.
mkdir -p "$h/Projects/flo/src" "$h/Projects/notes"
printf '#include <stdio.h>\n\nint main(void) {\n    puts("hello, Flo");\n    return 0;\n}\n' > "$h/Projects/flo/src/main.c"
printf '# Flo\n\nA light file manager.\n\n- folders\n- previews\n' > "$h/Projects/flo/README.md"
printf 'def greet(name):\n    return f"hi {name}"\n\nprint(greet("Flo"))\n' > "$h/Projects/flo/hello.py"
printf 'Shopping\n- bread\n- coffee\n- oranges\n' > "$h/Projects/notes/list.txt"
head -c 4000 /dev/urandom > "$h/Projects/flo/blob.bin"
tar -czf "$h/Projects/flo/backup.tar.gz" -C "$h/Projects/notes" list.txt
SETUP
chmod +x "$setup"
r=$(FLO_SESSION_THEME=${1:-} FLO_SESSION_SETUP=$setup sh "$here/tools/gui-session.sh" start $d "" 2>/dev/null | tail -1)
export DISPLAY=":$d"
shot() { $X sleep "$2" shot "$r/raw-$1.png"; magick "$r/raw-$1.png" -crop 1100x700+0+20 +repage "$out/$1.png"; }
sleep 3
# Connect dialog: the rail's Network section, "Connect…" row via its menu.
$X key ctrl+l sleep 0.3 type ":Connect" key Return sleep 0.6 key Return; shot connect-dialog 3
$X key Down sleep 0.2 key Down; shot connect-dialog-pick 0.6
$X key Escape sleep 0.5
# Folder cards with the sigma mark (Groups view, sizes measured).
$X key ctrl+l sleep 0.3 key ctrl+a type "$r/home/Projects" key Return sleep 1 key ctrl+1 sleep 0.4 key ctrl+shift+space; shot sigma-cards 3
# The views with their icons and text previews.
$X key ctrl+l sleep 0.3 key ctrl+a type "$r/home/Projects/flo" key Return sleep 1 key ctrl+2; shot icons-list 2
$X key ctrl+5; shot icons-grid 2
$X key ctrl+3; shot icons-columns 1.5
$X key ctrl+1; shot icons-groups 1.5
# Preferences > Default applications.
$X key ctrl+comma sleep 0.5 key End sleep 0.2 key Right; shot preferences-defaults 1
$X key Escape sleep 0.2 key Escape
sh "$here/tools/gui-session.sh" stop "$r"
echo "$r"
