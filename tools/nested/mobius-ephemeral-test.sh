#!/bin/bash
# Ephemeral UI: a reply's fenced `ui` blocks are drawn as choices, a form and figures; what the person picks or fills in goes back as a message.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-ephemeral-test.sh tools/nested/run.sh mobius-ephemeral tools/nested/mobius-nested.sh <wide window> "HELIX_FAKE_REPLY=..."
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
idle() { sleep 2; for _ in $(seq 1 120); do ! has_id stop && break; sleep 0.5; done; sleep 1.5; }
users() { python3 - "$CHATS" <<'PY'
import glob, json, sys
f = glob.glob(sys.argv[1] + "/*.json")
print("|".join(m["text"].replace("\n", "/") for m in json.load(open(f[0]))["messages"] if m["role"] == "user") if f else "")
PY
}
click_id input; wtype "Help me decide"; wtype -k Return; idle
wait_id eph-choices && wait_id eph-form && wait_id eph-stats && echo "ok: choices, form and figures are drawn" || fail "missing: choices/form/stats"
shot ephemeral; sleep 1; shot ephemeral
echo "--- picking an option sends it as a message"
click_id eph-option-1; idle
[ "$(users)" = "Help me decide|Blue" ] && echo "ok: the choice went back as a message" || fail "messages: $(users)"
echo "--- filling the form sends the values"
click_id eph-field-0; wtype "Ada"; click_id eph-submit; idle
case "$(users)" in *"Your details/- Name: Ada/- Level: Beginner"*) echo "ok: the form went back as a message";; *) fail "messages: $(users)";; esac
[ "$FAILED" = 0 ] && echo "PASS: mobius ephemeral" || exit 1
