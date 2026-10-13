#!/bin/bash
# What the shell's processes hold (PSS) idle and with the notification shade open. Used to compare page builds.
#   HELIX_SHELL_BIN=<binary> HELIX_NESTED_SCRIPT=tools/nested/measure-shell-test.sh tools/nested/run.sh measure-shell tools/nested/tauri-shell.sh
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control
rss() {
  local total=0 p
  for p in $(pgrep -f "WebKit|helix-shell|helix-service"); do
    tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -q "^XDG_STATE_HOME=$SCRATCH/state$" || continue
    total=$((total + $(awk '/^Pss:/{s+=$2} END{print s+0}' /proc/$p/smaps_rollup 2>/dev/null)))
  done
  echo $total
}
for _ in $(seq 1 120); do grep -q "LIVE Sound" "$LOG" && break; sleep 0.25; done; sleep 4
echo "idle: $(rss) KiB (PSS)"
echo notifications >> "$CONTROL"
for _ in $(seq 1 40); do grep -q "PANEL open" "$LOG" && break; sleep 0.25; done; sleep 3
echo "shade open: $(rss) KiB (PSS)"
