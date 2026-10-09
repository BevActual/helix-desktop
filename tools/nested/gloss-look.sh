#!/bin/bash
# A confirm-tier window-close request, with a real window open, on a protected compositor: the dialog says which window the id is.
#   tools/nested/trusted-run.sh protected gloss-look.sh
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log
export LAYOUT=$SCRATCH/state/layout-confirmer.json
source "$ROOT/tools/nested/ui-lib.sh"
count() { grep -c "$1" "$LOG" 2>/dev/null || true; }
wait_for() { for _ in $(seq 1 "${3:-80}"); do [ "$(count "$1")" -ge "$2" ] && return 0; sleep 0.25; done; return 1; }
wait_for "LIVE Sound" 1 120 || exit 1; wait_for "CONFIRMER registered" 1 80 || exit 1
APP_ID=glasswin TARGET_FILE="$SCRATCH/state/glass.txt" python3 "$ROOT/tools/nested/text-target.py" >/dev/null 2>&1 & WIN=$!
sleep 3
ID=$(niri msg --json windows | python3 -c "import json,sys; print([w['id'] for w in json.load(sys.stdin) if w['app_id']=='glasswin'][0])")
"$HELIX_PROBE_PYTHON" "$ROOT/tools/nested/agent-probe.py" act window-close "$ID" >"$SCRATCH/probe.out" 2>&1 & PROBE=$!
wait_for "CONFIRMER armed" 1 40 || echo "no dialog"
sleep 1; niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5
cp "$(ls -t "$SCRATCH"/shots/*.png | head -1)" "$SCRATCH/gloss.png"
wtype -k Escape; sleep 1; kill $PROBE $WIN 2>/dev/null
echo done
