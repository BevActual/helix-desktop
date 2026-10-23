#!/bin/bash
# The greeter's Restart and Shut down: reached from the keyboard, each asks first (Cancel has the focus), and only a second, deliberate step does it.
# The greeter writes the action to a file instead of acting (HELIX_GREETER_POWER_LOG). After a login the account and session are remembered.
#   HELIX_NESTED_SCRIPT=tools/nested/greeter-power-test.sh tools/nested/run.sh greeter-power tools/nested/greeter-nested.sh
SCRATCH=$1; FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
POWER=$SCRATCH/state/power.log; LAST=$SCRATCH/state/last.json; LOG=$SCRATCH/state/greetd-log.jsonl
sleep 5
echo "--- Restart: ask, cancel, ask again, do"
wtype -k Tab; wtype -k Tab; sleep 0.4   # Not you? -> Restart (the field has the focus first)
wtype -k Return; sleep 0.8; shot power-ask
[ ! -e "$POWER" ] && echo "ok: choosing Restart did nothing yet" || fail "acted at once: $(cat "$POWER")"
wtype -k Return; sleep 0.8   # Cancel has the focus
[ ! -e "$POWER" ] && echo "ok: Cancel went back without doing anything" || fail "cancel acted: $(cat "$POWER")"
wtype -k Return; sleep 0.8   # Cancel put the focus back on Restart: choose it again
shot power-ask2
wtype -M shift -k Tab -m shift; sleep 0.3; wtype -k Return; sleep 1
[ "$(cat "$POWER" 2>/dev/null)" = "reboot" ] && echo "ok: the second, deliberate step restarted" || fail "reboot: $(cat "$POWER" 2>&1)"
echo "--- a login is remembered"
wtype -M shift -k Tab -m shift; wtype -M shift -k Tab -m shift; sleep 0.4   # back to the password field
for _ in $(seq 1 40); do grep -q create_session "$LOG" 2>/dev/null && break; sleep 0.25; done
wtype "correct horse"; wtype -k Return; sleep 3
python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); sys.exit(0 if d["user"] and d["session"] is not None else 1)' "$LAST" && echo "ok: the account is remembered ($(cat "$LAST"))" || fail "not remembered: $(cat "$LAST" 2>&1)"
[ $FAILED = 0 ] && echo "PASS: greeter power" || exit 1
