#!/bin/bash
# Follow-ups typed while a reply is being written wait in a list above the composer (one row each, Remove and Send now), and go one after another when
# the reply ends. A slow scripted backend leaves time to type them.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-queue-test.sh tools/nested/run.sh mobius-queue tools/nested/mobius-nested.sh <wide window> HELIX_FAKE_DELAY_MS=600
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
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
texts() { python3 - "$CHATS" <<'PY'
import glob, json, sys
files = glob.glob(sys.argv[1] + "/*.json")
print("|".join(m["text"] for m in json.load(open(files[0]))["messages"] if m["role"] == "user") if files else "")
PY
}
echo "--- 1. follow-ups wait in a list while the reply is written"
click_id input; wtype "First question"; wtype -k Return
wait_id stop && echo "ok: the reply is being written" || fail "no reply started"
click_id input; wtype "Second question"; wtype -k Return; sleep 0.4
click_id input; wtype "Third question"; wtype -k Return; sleep 0.4
click_id input; wtype "Fourth question"; wtype -k Return; sleep 0.4
wait_id queue-item-0 && wait_id queue-item-1 && wait_id queue-item-2 && echo "ok: each follow-up has its own row (none replaced another)" || fail "queue rows missing"
echo "--- 2. one can be removed"
click_id queue-remove-1; sleep 0.5
has_id queue-item-2 && fail "the removed row is still there" || echo "ok: removing a row leaves the others"
echo "--- 3. the rest are sent in order when the reply ends"
for _ in $(seq 1 300); do [ "$(texts)" = "First question|Second question|Fourth question" ] && ! has_id stop && break; sleep 0.5; done
[ "$(texts)" = "First question|Second question|Fourth question" ] && echo "ok: sent in the order typed, without the removed one" || fail "sent: $(texts)"
has_id queue-item-0 && fail "the list did not empty" || echo "ok: the list is empty again"
[ "$FAILED" = 0 ] && echo "PASS: mobius queue" || exit 1
