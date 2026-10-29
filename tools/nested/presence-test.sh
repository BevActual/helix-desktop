#!/bin/bash
# The launcher as the one place to ask (docs/design/north-star.md, "one presence"): typing what a setting does offers the setting, and
# Enter flips a switch there (the person's own change, recorded as "You" with Undo); anything typed can be asked of Mobius (the last row,
# and the only one when nothing else matches), which opens Mobius in the bar with the words; "act …" still lists desktop actions.
#   tools/nested/ui-suite.sh presence
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control
fail() { echo "FAIL: $*" >&2; FAILED=1; }
wait_for() { for _ in $(seq 1 "${3:-60}"); do [ "$(grep -c "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0; sleep 0.25; done; echo "TIMEOUT waiting for $2 x $1"; return 1; }
shot() { sleep 1; niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
rows() { grep "LAUNCHER results" "$LOG" | tail -1; }
config() {
  python3 - "$@" <<'PY'
import os, sys, gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib
c = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
args = GLib.Variant.parse(None, sys.argv[2], None, None) if sys.argv[2] else None
out = c.call_sync("org.helix.Desktop", "/org/helix/Desktop", "org.helix.Desktop.Configuration1", sys.argv[1], args,
                  GLib.VariantType(sys.argv[3]), 0, 10000, None)
print(out.unpack()[0])
PY
}
value_of() { config GetSettings "" "(av)" | python3 -c "
import sys, ast
rows = ast.literal_eval(sys.stdin.read())
print(next((r.get('value') for r in rows if r.get('id') == '$1'), 'missing'))"; }
wait_for "LIVE Sound" 1 120 || exit 1
sleep 2

echo "--- 1. a setting by what it does, flipped from the launcher"
BEFORE=$(value_of theme.pure_black_dark)
echo launcher >> "$CONTROL"; wait_for "PANEL open" 1 || fail "launcher did not open"
sleep 0.8; wtype "pure black"; sleep 1.2; shot presence-setting
rows | grep -q "first helix:setting:theme.pure_black_dark" && echo "ok: the setting is the first row ($(rows))" || fail "setting not first: $(rows)"
python3 - "$SCRATCH/state/layout.json" <<'PY' && echo "ok: Ask Mobius is offered too, as the last row" || fail "no Ask Mobius row"
import json, sys
e = json.load(open(sys.argv[1]))["elements"]
assert "launcher-row:helix:ask" in e and "launcher-row:helix:setting:theme.pure_black_dark" in e
assert e["launcher-row:helix:ask"]["y"] > e["launcher-row:helix:setting:theme.pure_black_dark"]["y"]
PY
wtype -k Return
wait_for "LAUNCHER setting theme.pure_black_dark" 1 20 && echo "ok: Enter flipped it" || fail "no setting change logged"
for _ in $(seq 1 20); do [ "$(value_of theme.pure_black_dark)" != "$BEFORE" ] && break; sleep 0.25; done
[ "$(value_of theme.pure_black_dark)" != "$BEFORE" ] && echo "ok: the desktop has it" || fail "value unchanged"
config ActionHistory "(uint32 1,)" "(s)" | python3 -c "
import sys, json
e = json.loads(sys.stdin.read())[0]
assert e['target'] == 'theme.pure_black_dark' and e['by']['label'] == 'You', e
print('ok: recorded as yours:', e['sentence'])" || fail "not in the history as You"

echo "--- 2. anything else is asked of Mobius"
echo launcher >> "$CONTROL"; wait_for "PANEL open" 2 || fail "launcher did not open again"
sleep 0.8; wtype "why is my laptop fan so loud"; sleep 1.2; shot presence-ask
rows | grep -q "results 1 first helix:ask" && echo "ok: with nothing else matching, Ask Mobius is the only row" || fail "rows: $(rows)"
wtype -k Return
wait_for "LAUNCHER ask" 1 20 && echo "ok: Enter asked Mobius" || fail "not asked"
wait_for "PANEL open" 3 20 && echo "ok: Mobius opened in the bar" || fail "Mobius did not open: $(grep 'PANEL' "$LOG" | tail -3)"
grep -q "panicked" "$LOG" && fail "the shell panicked: $(grep -A1 panicked "$LOG" | head -2)" || echo "ok: nothing panicked"
sleep 1; shot presence-asked

grep -q "^ASK answer" "$LOG" && echo "ok: a question is answered (not done)" || fail "the question was not routed to answering: $(grep '^ASK' "$LOG")"

echo "--- 3. act still lists desktop actions"
echo escape >> "$CONTROL"; sleep 1
n=$(grep -c "PANEL open" "$LOG"); echo launcher >> "$CONTROL"; wait_for "PANEL open" $((n + 1)) 20; sleep 0.8; wtype "act dark"; sleep 1.2;
rows | grep -q "first helix:desktop:" && echo "ok: act lists desktop actions" || fail "act: $(rows)"
rows | grep -q "helix:ask" && fail "Ask Mobius should not be offered in act mode" || true
echo escape >> "$CONTROL"
echo "--- 2b. a request to do something is done, from the same box"
echo escape >> "$CONTROL"; sleep 1
n=$(grep -c "PANEL open" "$LOG"); echo launcher >> "$CONTROL"; wait_for "PANEL open" $((n + 1)) 20; sleep 0.8; wtype "please lower the volume a little"; sleep 1.2
rows | grep -q "first helix:ask" && wtype -k Return
for _ in $(seq 1 20); do grep -q "^ASK do" "$LOG" && break; sleep 0.25; done
grep -q "^ASK do" "$LOG" && echo "ok: \"please lower the volume a little\" went to doing" || fail "not routed to doing: $(grep '^ASK' "$LOG")"
grep -q "panicked" "$LOG" && fail "the shell panicked" || true
# (Doing starts the desktop assistant, which asks the person for access in a dialog that holds the keyboard: so this comes last.)

[ "$FAILED" = 0 ] && echo "PASS: presence" || exit 1
