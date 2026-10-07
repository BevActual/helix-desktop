#!/bin/bash
# HELIX_NESTED_SCRIPT for M1/M2. $1 is the scratch directory.
# Updates audio.sound_volume on the private bus, then opens and dismisses the panel through the surface manager.
set -u
SCRATCH=$1
LOG=$SCRATCH/program.log
CONTROL=$SCRATCH/state/surface-control
PID=$(cat "$SCRATCH/state/shell.pid")
HERE=$(cd "$(dirname "$0")" && pwd)
wait_count() {
  local needle=$1 need=$2
  for _ in $(seq 1 80); do
    local n
    n=$(grep -c "$needle" "$LOG" 2>/dev/null || true)
    [ "$n" -ge "$need" ] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $need x $needle" >&2
  return 1
}
cleanup() {
  if [ -f "$SCRATCH/state/helix-service.pid" ]; then
    kill "$(cat "$SCRATCH/state/helix-service.pid")" 2>/dev/null || true
  fi
}
trap cleanup EXIT
wait_count "LIVE Sound " 1
echo "--- M1 bar idle, before the value changes ---"
python3 "$HERE/process_tree.py" "$PID" "bar-only-before"
sleep 8
python3 "$HERE/process_tree.py" "$PID" "bar-only-after"
gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Configuration1.Update audio.sound_volume '<0.25>'
wait_count "LIVE Sound 25%" 1
echo open >> "$CONTROL"
wait_count "PANEL open" 1
sleep 1
niri msg action screenshot-screen >/dev/null 2>&1
sleep 1.5
ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/panel-open.png"
python3 "$HERE/coverage.py" "$SCRATCH/panel-open.png" "panel-open"
python3 "$HERE/process_tree.py" "$PID" "panel-open"
echo escape >> "$CONTROL"
wait_count "PANEL closed" 1
echo open >> "$CONTROL"
wait_count "PANEL open" 2
echo outside >> "$CONTROL"
wait_count "PANEL closed" 2
sleep 0.5
niri msg action screenshot-screen >/dev/null 2>&1
sleep 1.5
ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/panel-closed.png"
python3 "$HERE/coverage.py" "$SCRATCH/panel-closed.png" "panel-closed"
echo "--- READY lines ---"
grep "READY " "$LOG" || true
echo "--- LIVE lines ---"
grep "LIVE " "$LOG" || true
echo "--- PANEL lines ---"
grep "PANEL " "$LOG" || true
