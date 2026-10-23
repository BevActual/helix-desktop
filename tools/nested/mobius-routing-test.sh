#!/bin/bash
# A job on OpenRouter is routed as its options say: the request carries the fallback list, the price preference and the privacy rule; the reply
# names the model that really answered and what it cost; and the options can be changed in Settings and are used by the next message.
#   tools/nested/ui-suite.sh mobius-routing
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
for _ in $(seq 1 80); do [ -f "$SCRATCH/state/layout.json" ] && has_id input && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|} UI_REVEAL="760 150"
REQ=$SCRATCH/state/or-requests.jsonl
ROUTES=$SCRATCH/data/helix/mobius/cloud-routes.json
echo "--- 1. a message is routed as the options say"
click_id input; wtype "Hello there"; wtype -k Return
for _ in $(seq 1 60); do [ -s "$REQ" ] && break; sleep 0.5; done
python3 - "$REQ" <<'PY' && echo "ok: the request carried the fallbacks, the price preference, the privacy rule and cost reporting" || fail "request not as expected"
import json, sys
body = json.loads(open(sys.argv[1]).read().splitlines()[0])
assert body["models"] == ["acme/primary", "acme/backup"], body
assert "model" not in body, body
assert body["provider"] == {"data_collection": "deny", "sort": "price"}, body["provider"]
assert body["usage"] == {"include": True}, body
assert body["stream"] is True  # the gateway streams; a server that ignores it is still understood
PY
for _ in $(seq 1 40); do has_id stats-1 && break; sleep 0.5; done
has_id stats-1 && echo "ok: the reply has its details line" || fail "no details line"
F=$(ls -t "$SCRATCH"/data/helix/mobius/chats/*.json | head -1)
python3 - "$F" <<'PY' && echo "ok: the reply names the model that answered and its cost" || fail "stats not saved"
import json, sys
s = json.load(open(sys.argv[1]))["messages"][-1]["stats"]
assert s["model"] == "acme/backup" and s["cost_micro"] == 1200 and s["tokens"] == 7, s
PY
shot routing-reply; shot routing-reply
echo "--- 2. change the options in Settings"
click_id open-models; sleep 1; wait_id job-route-toggle-chat || fail "no routing options under Chat"
click_id job-route-toggle-chat; sleep 0.6
shot routing-options; shot routing-options
click_id job-prefer-chat-speed; sleep 0.8
click_id job-private-chat; sleep 0.8
click_id job-fb-remove-chat-0; sleep 0.8
click_id job-pick-fb-chat; sleep 0.6
wait_id job-fb-add-chat-acme/other && click_id job-fb-add-chat-acme/other; sleep 1
python3 - "$ROUTES" <<'PY' && echo "ok: the options were saved as they changed" || fail "options not saved: $(cat "$ROUTES")"
import json, sys
c = json.load(open(sys.argv[1]))["chat"]
assert c == {"prefer": "speed", "fallbacks": ["acme/other"], "private_only": False}, c
PY
echo "--- 3. the next message uses them"
click_id models-back; sleep 1.2
click_id input; wtype "Again please"; wtype -k Return
for _ in $(seq 1 60); do [ "$(wc -l < "$REQ")" -ge 2 ] && break; sleep 0.5; done
python3 - "$REQ" <<'PY' && echo "ok: the second request followed the new options" || fail "second request not as expected"
import json, sys
body = json.loads(open(sys.argv[1]).read().splitlines()[1])
assert body["models"] == ["acme/primary", "acme/other"], body
assert body["provider"] == {"sort": "throughput"}, body["provider"]
PY
[ $FAILED = 0 ] && echo "PASS: mobius routing" || exit 1
