#!/bin/sh
# Prints PSS and RSS (KiB, from /proc/PID/smaps_rollup) of a Flo process
# and, separately, of its child processes (thumbnailer workers, preview).
#   tools/measure-mem.sh FLO_PID
# Read-only: it only reads /proc.
set -eu
pid=$1
field() { awk -v k="$2:" '$1 == k { print $2 }' "/proc/$1/smaps_rollup" 2>/dev/null || echo 0; }
printf '%-8s %-18s %8s %8s\n' PID NAME PSS_KiB RSS_KiB
printf '%-8s %-18s %8s %8s\n' "$pid" "$(cat /proc/$pid/comm)" "$(field $pid Pss)" "$(field $pid Rss)"
kids=$(cat /proc/$pid/task/*/children 2>/dev/null | tr ' ' '\n' | grep . || true)
total_p=0
total_r=0
for k in $kids; do
  p=$(field $k Pss)
  r=$(field $k Rss)
  printf '%-8s %-18s %8s %8s\n' "$k" "$(cat /proc/$k/comm 2>/dev/null)" "$p" "$r"
  total_p=$((total_p + p))
  total_r=$((total_r + r))
done
echo "children total: PSS ${total_p} KiB, RSS ${total_r} KiB"
