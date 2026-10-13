#!/bin/bash
# Mobius dictation: the microphone button listens, then the words are transcribed locally and put in the composer.
# The recording is a file played as the microphone, and the service's scripted speech engine answers with what it received.
#   tools/nested/ui-suite.sh mobius-dictate
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
LOG=$SCRATCH/program.log
for _ in $(seq 1 80); do [ -f "$SCRATCH/state/layout.json" ] && has_id input && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
has_id dictate && echo "ok: the composer has a microphone button" || fail "no microphone button"
click_id dictate; sleep 2
click_id dictate; sleep 1
for _ in $(seq 1 60); do grep -q "MOBIUS dictated" "$LOG" && break; sleep 0.25; done
grep -q "MOBIUS dictated 19 characters" "$LOG" && echo "ok: the words were transcribed and put in the composer" || fail "no transcription ($(grep 'MOBIUS dictated' "$LOG" | tail -1))"
[ $FAILED = 0 ] && echo "PASS" || echo "FAILED"
