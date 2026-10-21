#!/bin/bash
# helix-summon: the compositor writes down which window had the keyboard at the key press, raises Mobius, and Mobius shows what it was as a chip the
# person can remove, sending it with the next message only if they leave it. A second summon while Mobius is itself focused leaves no note.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-summon-test.sh tools/nested/run.sh mobius-summon tools/nested/mobius-nested.sh <wide window>
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
idle() { sleep 2; for _ in $(seq 1 120); do ! has_id stop && break; sleep 0.5; done; sleep 1.5; }
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
focused() { niri msg --json windows | python3 -c "import json,sys; w=[x for x in json.load(sys.stdin) if x['is_focused']]; print((w[0]['app_id'] or '') if w else '')"; }
summon() { python3 - <<'PY'
import os, socket
s = socket.socket(socket.AF_UNIX); s.connect(os.environ["NIRI_SOCKET"]); s.send(b'{"Action":{"HelixSummon":{}}}\n'); s.settimeout(5); s.recv(4096)
PY
}
echo "--- another window has the keyboard"
SETTINGS=$(ls -t "$ROOT/target/fast/helix-settings" "$ROOT/target/release/helix-settings" 2>/dev/null | head -1)
"$SETTINGS" >/dev/null 2>&1 &
OTHER=$!
for _ in $(seq 1 60); do [ "$(focused)" != "org.helix.Mobius" ] && [ -n "$(focused)" ] && break; sleep 0.5; done
APP=$(focused)
[ -n "$APP" ] && [ "$APP" != "org.helix.Mobius" ] && echo "ok: $APP has the keyboard" || fail "no other window took the keyboard (got '$APP')"
has_id summon-chip && fail "a chip before any summon"
echo "--- the key is pressed (with some text selected in the other window)"
wl-copy --primary "The quarterly numbers are down 4% on the plan." 2>/dev/null &
sleep 1
summon; sleep 3
[ "$(focused)" = "org.helix.Mobius" ] && echo "ok: Mobius was raised, not started again" || fail "Mobius not focused: $(focused)"
[ "$(niri msg --json windows | python3 -c "import json,sys; print(sum(1 for w in json.load(sys.stdin) if w['app_id']=='org.helix.Mobius'))")" = 1 ] && echo "ok: still one Mobius window" || fail "a second Mobius window"
wait_id summon-chip 30 && echo "ok: the window it was is shown as a chip" || fail "no summon chip"
shotp() { niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
wait_id summon-selection-chip 20 && echo "ok: and the text that was selected is a second chip" || fail "no selection chip"
shotp summon; sleep 1; shotp summon
echo "--- it goes with the next message, once"
click_id input; wtype "What was I doing?"; wtype -k Return; idle
python3 - "$SCRATCH/data/helix/mobius/chats" "$APP" <<'PY' && echo "ok: the message carried the window note" || fail "note not sent"
import glob, json, sys
c = json.load(open(glob.glob(sys.argv[1] + "/*.json")[0]))
u = [m["text"] for m in c["messages"] if m["role"] == "user"][-1]
assert "summoned you" in u and sys.argv[2] in u, u
assert "quarterly numbers are down 4%" in u, u
PY
has_id summon-chip && fail "the chip stayed after sending" || echo "ok: and the chip is gone"
has_id summon-selection-chip && fail "the selection chip stayed" || echo "ok: and so is the selection chip"
echo "--- summoning from Mobius itself leaves no note"
summon; sleep 2
has_id summon-chip && fail "Mobius became the subject of its own summon" || echo "ok: no note when Mobius already had the keyboard"
has_id summon-selection-chip && fail "a selection was read without a summon note" || echo "ok: and no selection is read then either"
echo "--- a selection chip can be removed, and then it is not sent"
wl-copy --primary "private words" 2>/dev/null & sleep 1
focus_other() { niri msg action focus-window --id "$(niri msg --json windows | python3 -c "import json,sys; print(next(w['id'] for w in json.load(sys.stdin) if w['app_id']!='org.helix.Mobius'))")" >/dev/null 2>&1; sleep 1; }
focus_other; summon; sleep 3
wait_id summon-selection-chip 20 && click_id summon-selection-chip; sleep 0.8
has_id summon-selection-chip && fail "the chip stayed after being clicked" || echo "ok: clicking the selection chip removes it"
click_id input; wtype "Anything?"; wtype -k Return; idle
python3 - "$SCRATCH/data/helix/mobius/chats" <<'PY' && echo "ok: what was removed was not sent" || fail "removed text was sent"
import glob, json, sys
c = json.load(open(glob.glob(sys.argv[1] + "/*.json")[0]))
u = [m["text"] for m in c["messages"] if m["role"] == "user"][-1]
assert "private words" not in u, u
PY
kill "$OTHER" 2>/dev/null
[ "$FAILED" = 0 ] && echo "PASS: mobius summon" || exit 1
