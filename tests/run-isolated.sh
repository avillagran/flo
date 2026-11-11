#!/bin/sh
# Runs a test command in a fresh private environment:
#   run-isolated.sh RUNS_DIR COMMAND [ARGS...]
# Creates RUNS_DIR/run-<time>-<pid> (0700) holding tmp/, home/, and the XDG
# directories, points TMPDIR, HOME and XDG_* at them, and keeps the directory
# afterwards as evidence. Nothing here deletes anything.
set -eu
runs=$1
shift
umask 077
mkdir -p "$runs"
runs=$(cd "$runs" && pwd -P)
run=$(mktemp -d "$runs/run-$(date +%Y%m%d-%H%M%S)-XXXXXX")
chmod 0700 "$run"
for d in tmp home home/.config home/.local home/.local/share home/.cache home/.local/state runtime; do
  mkdir -p "$run/$d"
done
export TMPDIR="$run/tmp"
export HOME="$run/home"
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_DATA_HOME="$HOME/.local/share"
export XDG_CACHE_HOME="$HOME/.cache"
export XDG_STATE_HOME="$HOME/.local/state"
export XDG_RUNTIME_DIR="$run/runtime"
export FLO_TEST_RUN="$run"
# No session bus: tests must not reach the user's desktop services.
export DBUS_SESSION_BUS_ADDRESS="unix:path=$run/runtime/no-bus"
export GIO_USE_VFS=local
export GVFS_DISABLE_FUSE=1
echo "flo test run: $run"
exec "$@"
