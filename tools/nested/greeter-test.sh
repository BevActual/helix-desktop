#!/bin/bash
# The greeter against a fake greetd: a wrong password is refused and can be retried, the right one starts the session.
#   HELIX_NESTED_SCRIPT=tools/nested/greeter-test.sh tools/nested/run.sh greeter tools/nested/greeter-nested.sh
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
LOG=$SCRATCH/state/greetd-log.jsonl
sleep 4; shot greeter-start
# A single account is chosen already and its password field has focus.
for _ in $(seq 1 40); do grep -q create_session "$LOG" 2>/dev/null && break; sleep 0.25; done
grep -q create_session "$LOG" && echo "ok: choosing an account opened a session" || { fail "no create_session"; shot greeter-nosession; }
sleep 1
echo "--- 1. wrong password"
wtype "nope"; wtype -k Return; sleep 1.5; shot greeter-wrong
grep -q '"response": "nope"' "$LOG" && echo "ok: the typed answer was sent" || fail "answer not sent"
echo "--- 2. try again, right password"
wtype -k Tab; wtype -k Return; sleep 1.5   # Try again
wtype "correct horse"; wtype -k Return; sleep 2.5
grep -q start_session "$LOG" && echo "ok: the session was started" || { fail "no start_session"; shot greeter-nostart; tail -5 "$LOG"; }
grep -q '"cmd"' "$LOG" && grep start_session "$LOG" | grep -q -v '"env": \["' && echo "ok: a listed command, empty environment" || fail "start request odd"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
