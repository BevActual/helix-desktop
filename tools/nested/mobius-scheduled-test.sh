#!/bin/bash
# Scheduled prompts: made in the sheet, written as a systemd timer and service (not started here), and run headless by the same command the
# timer uses; the answer is saved as a chat.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-scheduled-test.sh tools/nested/run.sh mobius-scheduled tools/nested/mobius-nested.sh <wide window> HELIX_SCHEDULED_NO_SYSTEMD=1
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
CHATS=$SCRATCH/data/helix/mobius/chats
for _ in $(seq 1 80); do has_id input && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri", "msg", "--json", "windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri", "msg", "--json", "outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0, 0]; f = l.get("window_offset_in_tile") or [0, 0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
gone_id() { for _ in $(seq 1 24); do has_id "$1" || return 0; sleep 0.25; done; return 1; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
SCHED=$SCRATCH/data/helix/mobius/scheduled.json
UNITS=$SCRATCH/config/systemd/user
click_id mode-scheduled
wait_id scheduled-panel && wait_id sched-name && echo "ok: the Scheduled sheet opened" || fail "no sheet"
click_id sched-name; wtype "Morning brief"
click_id sched-prompt; wtype "Say hello to me"
shot scheduled; sleep 1; shot scheduled
click_id sched-add; sleep 2
python3 - "$SCHED" <<'PY' && echo "ok: the task is saved" || fail "task not saved"
import json, sys
t = json.load(open(sys.argv[1]))
assert len(t) == 1 and t[0]["name"] == "Morning brief" and t[0]["when"]["kind"] == "daily" and t[0]["enabled"], t
PY
grep -q "OnCalendar=\*-\*-\* 08:00:00" "$UNITS"/helix-mobius-task-*.timer && grep -q "Persistent=true" "$UNITS"/helix-mobius-task-*.timer && echo "ok: a systemd timer was written" || fail "no timer: $(ls "$UNITS" 2>&1)"
grep -q -- "--run-scheduled" "$UNITS"/helix-mobius-task-*.service && ! grep -q "Say hello" "$UNITS"/helix-mobius-task-*.service && echo "ok: the service runs this program with the id, not the prompt" || fail "bad service"
echo "--- running it, as the timer would"
ID=$(python3 -c "import json;print(json.load(open('$SCHED'))[0]['id'])")
wait_id "sched-run-$ID" && click_id "sched-run-$ID"
for _ in $(seq 1 60); do python3 -c "import json,sys;sys.exit(0 if json.load(open('$SCHED'))[0]['last_chat'] else 1)" 2>/dev/null && break; sleep 0.5; done
python3 - "$SCHED" "$SCRATCH/data/helix/mobius/chats" <<'PY' && echo "ok: the answer was saved as a chat named after the task" || fail "no answer chat"
import json, sys, os
t = json.load(open(sys.argv[1]))[0]
assert t["last_run"] > 0 and not t["last_error"], t
c = json.load(open(os.path.join(sys.argv[2], t["last_chat"] + ".json")))
assert c["title"] == "Morning brief" and [m["role"] for m in c["messages"]] == ["user", "assistant"], c
assert c["messages"][0]["text"] == "Say hello to me" and c["messages"][1]["text"].strip(), c
PY
echo "--- turning it off and deleting it removes the units"
click_id "sched-toggle-$ID"; sleep 1
python3 -c "import json,sys;sys.exit(0 if not json.load(open('$SCHED'))[0]['enabled'] else 1)" && echo "ok: switched off" || fail "not off"
click_id "sched-delete-$ID"; sleep 1
ls "$UNITS"/helix-mobius-task-* >/dev/null 2>&1 && fail "units remain" || echo "ok: the units are gone"
[ "$FAILED" = 0 ] && echo "PASS: mobius scheduled" || exit 1
