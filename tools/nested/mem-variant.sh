#!/bin/bash
# HELIX_NESTED_SCRIPT: bar-only and panel-open memory plus whether it still paints. For comparing environment variants:
#   tools/nested/run.sh name tools/nested/tauri-shell.sh WEBKIT_DISABLE_DMABUF_RENDERER=1   (with HELIX_NESTED_SCRIPT set)
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); PID=$(cat "$SCRATCH/state/shell.pid"); CONTROL=$SCRATCH/state/surface-control; LOG=$SCRATCH/program.log
for _ in $(seq 1 80); do grep -q "LIVE Sound" "$LOG" 2>/dev/null && break; sleep 0.5; done; sleep 5
python3 "$ROOT/tools/nested/process_tree.py" "$PID" bar-only | tail -1 | sed 's/cpu-seconds so far.*//'
echo open >> "$CONTROL"; sleep 2.5
niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/open.png"
python3 "$ROOT/tools/nested/process_tree.py" "$PID" panel-open | tail -1 | sed 's/cpu-seconds so far.*//'
python3 "$ROOT/tools/nested/coverage.py" "$SCRATCH/open.png" paint 2>/dev/null | tail -1
kill "$(cat "$SCRATCH/state/helix-service.pid")" 2>/dev/null
