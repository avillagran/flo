#!/bin/sh
# Extra fixtures for tools/gui-session.sh, used as FLO_SESSION_SETUP:
#
#   FLO_SESSION_SETUP=tools/session-fixtures.sh tools/gui-session.sh start 61
#
# Everything lands in the run directory given as $1; nothing outside it is
# written, and every tool below is a fake that only logs what it was asked
# (RUN_DIR/tool-calls.log), so nothing reaches a real network or service.
#
# - bin/: fake `tailscale` (status --json: m2 and studio to browse, a phone
#   and an offline machine that Flo leaves out; file cp), `dropbox-cli` (status, sharelink) and `localsend`, first on PATH.
# - home/Dropbox with a file, so the Dropbox place and its status show.
# - A mountinfo fixture (FLO_TEST_MOUNTINFO) with a removable drive at
#   /run/media/flo/FLO USB (RUN_DIR/run-media inside the sandbox).
# - FLO_TEST_FAKE_GVFS: SFTP mounts land in RUN_DIR/runtime/gvfs (where
#   GVfs would put them) after the real GMountOperation prompts (password
#   "flo-test"); studio's has a few files.
# - Two private applications for JPEG pictures (for Open With), which log
#   the files they are asked to open.
set -eu
run=$1
h=$run/home
umask 077
mkdir -p "$run/bin" "$run/runtime/gvfs/sftp:host=studio.tail1.ts.net/srv/projects" "$h/Dropbox" "$h/.local/share/applications"
log=$run/tool-calls.log
: > "$log"

cat > "$run/bin/tailscale" <<EOF
#!/bin/sh
echo "tailscale \$*" >> "$log"
case "\$1 \${2:-}" in
"status --json")
  cat <<'JSON'
{"Self": {"HostName": "node", "DNSName": "node.tail1.ts.net.", "OS": "linux", "Online": true},
 "Peer": {
  "k1": {"HostName": "m2", "DNSName": "m2.tail1.ts.net.", "OS": "macOS", "Online": true, "TailscaleIPs": ["100.64.0.2"]},
  "k2": {"HostName": "studio", "DNSName": "studio.tail1.ts.net.", "OS": "linux", "Online": true, "TailscaleIPs": ["100.64.0.3"]},
  "k3": {"HostName": "phone", "DNSName": "phone.tail1.ts.net.", "OS": "iOS", "Online": true},
  "k4": {"HostName": "old-desk", "DNSName": "old-desk.tail1.ts.net.", "OS": "linux", "Online": false}
 }}
JSON
  ;;
"file cp") exit 0 ;;
*) exit 1 ;;
esac
EOF

cat > "$run/bin/dropbox-cli" <<EOF
#!/bin/sh
echo "dropbox-cli \$*" >> "$log"
case "\$1" in
status) echo "Up to date" ;;
sharelink) echo "https://www.dropbox.com/scl/fi/flotest/\$(basename "\$2")?dl=0" ;;
*) exit 1 ;;
esac
EOF

cat > "$run/bin/localsend" <<EOF
#!/bin/sh
echo "localsend \$*" >> "$log"
EOF
chmod 0700 "$run/bin/tailscale" "$run/bin/dropbox-cli" "$run/bin/localsend"

printf 'Remote notes\n' > "$run/runtime/gvfs/sftp:host=studio.tail1.ts.net/srv/README"
printf 'Shared with the team\n' > "$h/Dropbox/plan.txt"
cp "$h/Pictures/Lisbon 2026/IMG_0001.jpg" "$h/Dropbox/cover.jpg"

# A removable drive mounted at /run/media/flo/FLO USB (inside the sandbox).
mkdir -p "$run/run-media/flo/FLO USB/DCIM"
printf 'On the stick\n' > "$run/run-media/flo/FLO USB/notes.txt"
cp "$h/Pictures/Lisbon 2026/IMG_0002.jpg" "$run/run-media/flo/FLO USB/DCIM/"
cat > "$run/mountinfo" <<'EOF'
22 1 259:2 / / rw,relatime shared:1 - btrfs /dev/nvme0n1p2 rw
23 22 0:5 / /proc rw,nosuid shared:2 - proc proc rw
40 22 8:17 / /run/media/flo/FLO\040USB rw,nosuid,nodev,relatime shared:9 - vfat /dev/sdb1 rw
EOF

cat > "$run/bin/flo-fixture-open" <<EOF
#!/bin/sh
echo "\$*" >> "$run/opened.log"
EOF
chmod 0700 "$run/bin/flo-fixture-open"
for app in viewer:Fixture\ Viewer paint:Fixture\ Paint; do
  id=${app%%:*}
  name=${app#*:}
  cat > "$h/.local/share/applications/flo-fixture-$id.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$name
Exec=flo-fixture-open $id %f
MimeType=image/jpeg;image/png;
EOF
done
# GIO finds an application's types through mimeinfo.cache, as on a real
# desktop after update-desktop-database.
update-desktop-database "$h/.local/share/applications"

cat >> "$run/env.sh" <<EOF
export PATH="$run/bin:\$PATH"
export FLO_TEST_MOUNTINFO="$run/mountinfo" FLO_TEST_FAKE_GVFS="$run/runtime/gvfs"
EOF
