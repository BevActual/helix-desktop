#!/bin/bash
# The dock honours the settings file Settings writes: always showing (no auto-hide), a smaller icon size, and back again without a restart.
#   HELIX_NESTED_SCRIPT=tools/nested/dock-prefs-test.sh tools/nested/run.sh dock-prefs tools/nested/dock-nested.sh
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.."  && pwd); LOG=$SCRATCH/program.log; FAILED=0
LAYOUT=$SCRATCH/state/layout.json
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
FILE=$SCRATCH/config/helix/dock-settings.json
for _ in $(seq 1 120); do grep -q "LIVE Sound" "$LOG" && break; sleep 0.25; done; sleep 2
size_of() { python3 - "$LAYOUT" "$1" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
e = d["elements"].get(sys.argv[2])
# A hidden dock is parked below the screen: it is reported, but nobody can see it.
print(round(e["w"]) if e and e["y"] < d["height"] - 4 else "none")
PY
}
echo "--- 1. by default the dock is hidden until the pointer reaches the edge"
[ "$(size_of dock-item-0)" = none ] && echo "ok: hidden" || fail "the dock showed by itself: $(size_of dock-item-0)"
echo "--- 2. always showing, smaller icons, no dots: no restart"
mkdir -p "$(dirname "$FILE")"
echo '{"version":1,"auto_hide":false,"icon_size":40,"indicators":false,"magnify":false}' > "$FILE"
for _ in $(seq 1 20); do [ "$(size_of dock-item-0)" != none ] && break; sleep 0.5; done
[ "$(size_of dock-item-0)" = 40 ] && echo "ok: showing, 40 wide" || fail "expected a 40 px icon, saw $(size_of dock-item-0)"
sleep 1
niri msg layers 2>/dev/null | grep -q helix-shell-dock-reserve && echo "ok: the compositor keeps space for the dock" || fail "no space reserved"
grep -q "DOCK settings changed" "$LOG" && echo "ok: the shell noticed the change" || fail "no change noticed"
echo "--- 2b. always showing but not reserving"
echo '{"version":1,"auto_hide":false,"reserve_space":false}' > "$FILE"; sleep 3
niri msg layers 2>/dev/null | grep -q helix-shell-dock-reserve && fail "space still reserved" || echo "ok: space let go"
[ "$(size_of dock-item-0)" != none ] && echo "ok: still showing" || fail "dock hid"
echo "--- 3. back to the defaults"
echo '{"version":1}' > "$FILE"
for _ in $(seq 1 20); do [ "$(size_of dock-item-0)" = none ] && break; sleep 0.5; done
[ "$(size_of dock-item-0)" = none ] && echo "ok: hides again" || fail "still showing: $(size_of dock-item-0)"
niri msg layers 2>/dev/null | grep -q helix-shell-dock-reserve && fail "space kept for a hidden dock" || echo "ok: nothing reserved for an auto-hiding dock"
echo "--- 4. a damaged file means the defaults"
echo 'not json' > "$FILE"; sleep 2.5
[ "$(size_of dock-item-0)" = none ] && echo "ok: still the defaults" || fail "damaged file changed the dock"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
