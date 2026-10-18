#!/bin/bash
# A chat is named after its first exchange by the model that answered: the saved title changes from the first message to what the model wrote, only once,
# and a second message does not rename it. The scripted backend answers every request with HELIX_FAKE_REPLY, which here is a plausible title.
#   HELIX_NESTED_AUTO_TITLE=1 HELIX_FAKE_REPLY="Cron versus systemd timers" HELIX_NESTED_SCRIPT=tools/nested/mobius-title-test.sh \
#     tools/nested/run.sh mobius-title tools/nested/mobius-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }" HELIX_NESTED_AUTO_TITLE=1 HELIX_FAKE_REPLY="Cron versus systemd timers"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
CHATS=$SCRATCH/data/helix/mobius/chats
title() { python3 - "$CHATS" <<'PY'
import glob, json, sys
files = glob.glob(sys.argv[1] + "/*.json")
print(json.load(open(files[0]))["title"] if files else "")
PY
}
for _ in $(seq 1 80); do has_id composer && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri", "msg", "--json", "windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri", "msg", "--json", "outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0, 0]; f = l.get("window_offset_in_tile") or [0, 0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
echo "--- 1. the first exchange names the chat"
click_id composer; wtype "How does cron work, and when should I use a systemd timer instead?"; sleep 0.3
click_id send
for _ in $(seq 1 60); do grep -q 'MOBIUS titled the chat' "$SCRATCH/program.log" && break; sleep 0.5; done
grep -q 'MOBIUS titled the chat' "$SCRATCH/program.log" && echo "ok: the chat was named" || fail "never named: $(grep MOBIUS "$SCRATCH/program.log" | tail -3)"
[ "$(title)" = "Cron versus systemd timers" ] && echo "ok: saved with the model's title" || fail "title is '$(title)'"
echo "--- 2. a second message does not rename it"
click_id composer; wtype "And what about anacron?"; sleep 0.3; click_id send
sleep 6
[ "$(grep -c 'MOBIUS titled the chat' "$SCRATCH/program.log")" = 1 ] && [ "$(title)" = "Cron versus systemd timers" ] && echo "ok: named once" || fail "renamed: '$(title)'"
[ "$FAILED" = 0 ] && echo "PASS: mobius title" || exit 1
