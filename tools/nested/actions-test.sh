#!/bin/bash
# The actions catalogue (docs/design/actions.md) end to end against the real desktop service: a stand-in agent (pre-granted observe and
# act) finds a setting by what it does, reads it and changes it; the change is in the one history with the agent's name; a setting that
# runs a program is refused; a setting that needs the person is refused without the confirm grant; the person undoes the agent's
# change and the value is back; the person's own change from Settings is recorded as "You".
#   tools/nested/ui-suite.sh actions
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
probe() { python3 "$ROOT/tools/nested/agent-probe.py" "$@"; }
config() {  # method, GVariant-text args, reply type
  python3 - "$@" <<'PY'
import os, sys, gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib
c = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
args = GLib.Variant.parse(None, sys.argv[2], None, None) if sys.argv[2] else None
try:
    out = c.call_sync("org.helix.Desktop", "/org/helix/Desktop", "org.helix.Desktop.Configuration1", sys.argv[1], args,
                      GLib.VariantType(sys.argv[3]) if sys.argv[3] else None, 0, 10000, None)
    print(out.unpack()[0] if out is not None and len(out.unpack()) else "ok")
except GLib.Error as e:
    print("ERROR " + e.message)
PY
}
value_of() { config GetSettings "" "(av)" | python3 -c "
import sys, ast
rows = ast.literal_eval(sys.stdin.read())
print(next((r.get('value') for r in rows if r.get('id') == '$1'), 'missing'))"; }
for _ in $(seq 1 120); do busctl --user status org.helix.Desktop >/dev/null 2>&1 && break; sleep 0.25; done
sleep 3

echo "--- 1. find a setting by what it does"
out=$(probe action settings.find '{"query":"pure black"}')
echo "$out" | grep -q 'COMPLETED ok=True' && echo "$out" | grep -q 'theme.pure_black_dark' && echo "ok: found theme.pure_black_dark" || fail "find: $out"
echo "$out" | grep -q '\\"change\\": \\"act\\"\|\\"change\\":\\"act\\"' && echo "ok: it says an agent may change it" || echo "note: $(echo "$out" | head -c 300)"

echo "--- 2. read and change it"
BEFORE=$(value_of theme.pure_black_dark); echo "before: $BEFORE"
WANT=$([ "$BEFORE" = "True" ] && echo false || echo true)
out=$(probe action settings.set "{\"setting\":\"theme.pure_black_dark\",\"value\":$WANT}")
echo "$out" | grep -q 'COMPLETED ok=True' && echo "ok: changed ($(echo "$out" | grep -o 'Pure black[^"\\]*' | head -1))" || fail "set: $out"
AFTER=$(value_of theme.pure_black_dark)
[ "$AFTER" != "$BEFORE" ] && echo "ok: the store has it ($AFTER)" || fail "value unchanged: $AFTER"
out=$(probe action settings.set '{"setting":"theme.pure_black_dark","value":"purple"}')
echo "$out" | grep -q 'COMPLETED ok=False' && echo "$out" | grep -qi 'on or off' && echo "ok: a value it does not accept is refused in words" || fail "bad value: $out"

echo "--- 2b. the screen edges light while the agent acts"
edge() {  # prints "top-r top-g top-b left-r left-g left-b" of a fresh screenshot
  niri msg action screenshot-screen >/dev/null 2>&1; sleep 1
  python3 - "$(ls -t "$SCRATCH"/shots/*.png | head -1)" <<'PY'
import sys
from PIL import Image
img = Image.open(sys.argv[1]).convert("RGB")
w, h = img.size
print(*img.getpixel((w // 2, 1)), *img.getpixel((1, h // 2)))
PY
}
for _ in $(seq 1 20); do grep -q "^GLOW on" "$SCRATCH/program.log" && break; sleep 0.2; done
grep -q "^GLOW on" "$SCRATCH/program.log" && echo "ok: the shell lit the edges" || fail "no glow: $(grep GLOW "$SCRATCH/program.log")"
read -r tr tg tb lr lg lb <<<"$(edge)"
cp "$(ls -t "$SCRATCH"/shots/*.png | head -1)" "$SCRATCH/glow.png"
[ $((tr - tg)) -gt 40 ] && [ $((lr - lb)) -gt 40 ] && echo "ok: the top edge glows pink ($tr,$tg,$tb) and the left orange ($lr,$lg,$lb)" || fail "edges not lit: top $tr,$tg,$tb left $lr,$lg,$lb"

echo "--- 3. the history says who"
H=$(config ActionHistory "(uint32 10,)" "(s)")
echo "$H" | python3 -c "
import sys, json
h = json.loads(sys.stdin.read())
e = h[0]
assert e['target'] == 'theme.pure_black_dark', e
assert e['by']['kind'] == 'agent' and e['by']['label'] == 'nested activity probe', e['by']
assert e['sentence'].startswith('Pure black dark surfaces:'), e['sentence']
print('ok: history:', e['sentence'], 'by', e['by']['label'])" || fail "history: $H"
ENTRY=$(echo "$H" | python3 -c "import sys, json; print(json.loads(sys.stdin.read())[0]['id'])")

echo "--- 3b. the bar shows the agent's change, with Undo"
source "$ROOT/tools/nested/ui-lib.sh"; LAYOUT=$SCRATCH/state/layout.json
for _ in $(seq 1 40); do grep -q "ISLAND change $ENTRY" "$SCRATCH/program.log" && break; sleep 0.25; done
grep -q "ISLAND change $ENTRY" "$SCRATCH/program.log" && echo "ok: the shell picked up the change" || fail "no island change: $(grep ISLAND "$SCRATCH/program.log" | tail -3)"
for _ in $(seq 1 40); do grep -q "ISLAND Changed" "$SCRATCH/program.log" && break; sleep 0.25; done
grep -q "ISLAND Changed" "$SCRATCH/program.log" && echo "ok: the pill says what changed" || fail "pill not shown"
for _ in $(seq 1 40); do has_id island-undo && break; sleep 0.25; done
python3 - "$LAYOUT" <<'PY' && echo "ok: Undo sits in the pill" || fail "no Undo button in the pill"
import json, sys
e = json.load(open(sys.argv[1]))["elements"]
u, i = e["island-undo"], e["island"]
assert i["x"] <= u["x"] and u["x"] + u["w"] <= i["x"] + i["w"] + 1, (u, i)
PY
sleep 3   # once the pill has settled (it slides in over the clock)
niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.2; ls -t "$SCRATCH"/shots/*.png 2>/dev/null | head -1 | xargs -I{} cp {} "$SCRATCH/island-change.png"
click_id island-undo
for _ in $(seq 1 40); do grep -q "ISLAND undid $ENTRY" "$SCRATCH/program.log" && break; sleep 0.25; done
grep -q "ISLAND undid $ENTRY" "$SCRATCH/program.log" && echo "ok: Undo in the bar was pressed" || fail "undo from the bar: $(grep ISLAND "$SCRATCH/program.log" | tail -3)"
[ "$(value_of theme.pure_black_dark)" = "$BEFORE" ] && echo "ok: the value is back" || fail "not restored from the bar"
echo "--- 3c. the light goes out once the agents are done"
for _ in $(seq 1 40); do grep -q "^GLOW off" "$SCRATCH/program.log" && break; sleep 0.25; done
grep -q "^GLOW off" "$SCRATCH/program.log" && echo "ok: the light went out once the agent was done" || fail "glow stayed on"
sleep 0.5
read -r tr tg tb lr lg lb <<<"$(edge)"
[ $((tr - tg)) -lt 20 ] && [ $((lr - lb)) -lt 20 ] && echo "ok: the edges are dark again" || fail "edges still lit: top $tr,$tg,$tb left $lr,$lg,$lb"
# Change it again for the person's Undo in step 5.
out=$(probe action settings.set "{\"setting\":\"theme.pure_black_dark\",\"value\":$WANT}")
ENTRY=$(config ActionHistory "(uint32 1,)" "(s)" | python3 -c "import sys, json; print(json.loads(sys.stdin.read())[0]['id'])")

echo "--- 4. what an agent may not do"
out=$(probe action settings.set '{"setting":"hooks.started","value":"touch /tmp/owned"}')
echo "$out" | grep -q 'COMPLETED ok=False' && echo "$out" | grep -q 'may not change this' && echo "ok: a setting that runs a program is refused" || fail "hook: $out"
out=$(probe action settings.set '{"setting":"lockscreen.enabled","value":false}')
echo "$out" | grep -q 'COMPLETED ok=False' && echo "$out" | grep -q 'no confirm grant' && echo "ok: the lock screen needs the person (no confirm grant)" || fail "lock: $out"
out=$(probe action settings.nonsense '{}')
echo "$out" | grep -q 'COMPLETED ok=False' && echo "ok: an unknown action is refused" || fail "unknown: $out"

echo "--- 5. the person undoes it"
out=$(config UndoAction "(uint64 $ENTRY,)" "(s)")
echo "$out" | grep -q '^Undid Pure black' && echo "ok: $out" || fail "undo: $out"
[ "$(value_of theme.pure_black_dark)" = "$BEFORE" ] && echo "ok: the value is back" || fail "not restored: $(value_of theme.pure_black_dark)"
out=$(config UndoAction "(uint64 $ENTRY,)" "(s)")
echo "$out" | grep -q 'already undone' && echo "ok: it cannot be undone twice" || fail "twice: $out"

echo "--- 6. the person's own change is recorded as You"
config Update "('theme.pure_black_dark', <$WANT>)" "" >/dev/null
config ActionHistory "(uint32 5,)" "(s)" | python3 -c "
import sys, json
e = json.loads(sys.stdin.read())[0]
assert e['by'] == {'kind': 'person', 'label': 'You'}, e['by']
print('ok: recorded as', e['by']['label'])" || fail "person change not recorded as You"
[ -s "$SCRATCH/state/helix/actions/history.json" ] && echo "ok: the history is on disk" || fail "no history file"

[ "$FAILED" = 0 ] && echo "PASS: actions" || exit 1
