#!/bin/bash
# Helix Models: the overview shows the engine and where each slot goes; a backend is added with its key (the key reaches the secrets file, never the
# routing file), tested through the model service (which holds the key), and chosen for a slot; Act refuses a backend on another computer; the
# battery switch is remembered. Everything written is what the service reads on every request.
#   HELIX_NESTED_SCRIPT=tools/nested/helix-models-test.sh tools/nested/run.sh helix-models tools/nested/models-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
export UI_ORIGIN="0 0" UI_SCREEN="1280 720"
ROUTING=$SCRATCH/state/helix/inference-routing.toml
SECRET=$SCRATCH/state/helix/secrets/backend-studio
PORT=$(cat "$SCRATCH/state/stub.port")
for _ in $(seq 1 120); do has_id page-overview && break; sleep 0.25; done; sleep 2
echo "--- overview"
wait_id engine-row 20 && echo "ok: the engine is shown" || { shot noengine; fail "no engine row"; }
for slot in act chat summarize vision code; do has_id "slot-summary:$slot" || fail "no summary for $slot"; done && echo "ok: every slot is summarised"
has_id gateway-down && fail "the model service was not reached" || echo "ok: the model service answered"
echo "--- backends"
click_id page-backends; sleep 1
wait_id add-backend 20 && click_id add-backend; sleep 0.8
wait_id backend-sheet 10 && echo "ok: the add sheet opens" || fail "no sheet"
click_id backend-label; wtype "Studio"
click_id backend-id; wtype "studio"
click_id backend-base_url; wtype "http://127.0.0.1:$PORT/v1"
click_id backend-key; wtype "sk-nested"
shot before-save; click_id backend-save; sleep 2.5; shot after-save
grep -q 'id = "studio"' "$ROUTING" 2>/dev/null && echo "ok: the backend is in the routing file" || fail "not in routing: $(cat "$ROUTING" 2>&1)"
[ "$(cat "$SECRET" 2>/dev/null)" = "sk-nested" ] && echo "ok: the key is in the secrets file" || fail "no key file"
! grep -q "sk-nested" "$ROUTING" && echo "ok: and not in the routing file" || fail "the key leaked into the routing file"
[ "$(stat -c %a "$SECRET" 2>/dev/null)" = 600 ] && echo "ok: the key file is owner only" || fail "mode $(stat -c %a "$SECRET" 2>&1)"
wait_id probe-result:studio 20 && echo "ok: it was tested after saving, through the service" || { shot noprobe; fail "no probe result"; }
python3 - "$SCRATCH/layout-text.txt" <<'PY' 2>/dev/null; true
PY
click_id probe:studio; sleep 2
wait_id probe-result:studio 10 && echo "ok: Test shows a result" || fail "no result after Test"
echo "--- routing"
click_id page-routing; sleep 1
wait_id slot-backend:chat 20 && echo "ok: each slot has a backend choice" || fail "no routing rows"
# Talk goes to Studio: open the pop-up button and choose it from the menu.
click_id slot-backend:chat; sleep 0.6; wait_id slot-backend:chat-studio 10 && click_id slot-backend:chat-studio; sleep 1.5
grep -A2 '^\[profiles.chat\]' "$ROUTING" | grep -q 'backend = "studio"' && echo "ok: Talk now goes to Studio" || { shot noroute; fail "chat not routed: $(grep -A3 'profiles.chat' "$ROUTING")"; }
# Act cannot be sent to another computer: its menu item is disabled.
click_id slot-backend:act; sleep 0.6
has_id slot-backend:act-studio && python3 - "$SCRATCH/state/layout.json" <<'PY' && echo "ok: Act's menu is there" || fail "no act menu"
import json, sys
assert "slot-backend:act-studio" in json.load(open(sys.argv[1]))["elements"]
PY
click_id slot-backend:act-studio; sleep 1
! grep -A2 '^\[profiles.act\]' "$ROUTING" | grep -q 'studio' && echo "ok: Act stayed on this computer" || fail "act was routed to studio"
wtype -k Escape; sleep 0.5
# A model offered by the backend (found by the service when it was tested), then a name typed in the sheet.
click_id slot-model:chat; sleep 0.6; wait_id slot-model:chat-big-model 10 && click_id slot-model:chat-big-model; sleep 1.5
grep -A3 '^\[profiles.chat\]' "$ROUTING" | grep -q 'model = "big-model"' && echo "ok: a model the backend offers was chosen from its menu" || fail "model not saved: $(grep -A3 'profiles.chat' "$ROUTING")"
# The Safety group is below the fold of this window; the keyboard scrolls (the pointer harness cannot).
for _ in 1 2 3; do wtype -k End; sleep 0.5; done
click_id battery-remote; sleep 1.2
grep -q 'allow_remote_on_battery_saver = true' "$ROUTING" && echo "ok: the battery switch is remembered" || fail "battery not saved"
shot models-routing
echo "--- the service reads it"
python3 - <<'PY' && echo "ok: the service sees the same routing" || fail "service snapshot differs"
import subprocess, json
out = subprocess.check_output(["busctl", "--user", "--json=short", "call", "org.helix.Inference", "/org/helix/Inference", "org.helix.Inference1", "GetSnapshot"]).decode()
data = json.loads(out)["data"][0]
assert data["profiles"]["data"]["chat"]["data"]["backend"]["data"] == "studio", data["profiles"]
PY
[ "$FAILED" = 0 ] && echo "PASS: helix models" || exit 1
