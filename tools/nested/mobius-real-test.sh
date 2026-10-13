#!/bin/bash
# One real turn: a small prompt answered by the installed model. Skipped when no model is installed.
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
LOG=$SCRATCH/program.log
REAL_HOME=$(getent passwd "$(id -u)" | cut -d: -f6)
ls "$REAL_HOME"/.local/share/helix/models/*.gguf >/dev/null 2>&1 || { echo "skip: no local model installed"; exit 0; }
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
click_id input; wtype "In one short sentence, what is a cron job?"; wtype -k Return
START=$(date +%s)
for _ in $(seq 1 1200); do grep -q "MOBIUS turn ended" "$LOG" && break; sleep 0.5; done
echo "turn took $(( $(date +%s) - START )) s"
grep -a "MOBIUS turn ended" "$LOG" | tail -1
grep -q "MOBIUS turn ended Finished" "$LOG" && echo "ok: the model answered" || fail "the turn did not finish: $(tail -3 "$LOG" | cut -c1-200)"
F=$(ls -t "$SCRATCH"/data/helix/mobius/chats/*.json 2>/dev/null | head -1)
python3 - "$F" <<'PY' && echo "ok: a reply was saved" || echo "FAIL: no reply saved" >&2
import json,sys
d=json.load(open(sys.argv[1])); reply=[m["text"] for m in d["messages"] if m["role"]=="assistant"]
print("reply:", (reply[-1] if reply else "")[:200]); sys.exit(0 if reply and reply[-1].strip() else 1)
PY
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
