#!/bin/bash
# helix-nightlight against the compositor: it starts, finds the output, decides the screen should be warm from nightlight.json, tries to take the
# output's colour table (the test compositor, with HELIX_NESTED_GAMMA=1, offers a pretend table and logs what is set), and lets go again when the file turns
# it off. Whether the colours really change needs a real display; the ramp itself is unit tested in helix-nightlight-model.
#   HELIX_NESTED_SCRIPT=tools/nested/nightlight-test.sh tools/nested/run.sh nightlight tools/nested/nightlight-nested.sh
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
waitlog() { for _ in $(seq 1 "${2:-60}"); do grep -q "$1" "$LOG" 2>/dev/null && return 0; sleep 0.5; done; return 1; }
waitlog "NIGHTLIGHT started with 1 output" 40 && echo "ok: it started and found the output" || fail "did not start: $(tail -5 "$LOG")"
waitlog "NIGHTLIGHT settings enabled=true warmth=3000 scheduled=false" 10 && echo "ok: it read nightlight.json" || fail "settings not read"
waitlog "NIGHTLIGHT warm" 10 && echo "ok: it decided the screen should be warm" || fail "never went warm"
NL=$SCRATCH/niri.log
for _ in $(seq 1 20); do grep -q "HELIX gamma output=.* r=" "$NL" && break; sleep 0.5; done
python3 - "$NL" <<'PY' && echo "ok: the compositor was given a warm table (full red, less green, least blue)" || fail "no warm table: $(grep 'HELIX gamma' "$NL" | tail -3)"
import re, sys
lines = [l for l in open(sys.argv[1]) if "HELIX gamma output=" in l and " r=" in l]
r, g, b = (int(x) for x in re.search(r"r=(\d+) g=(\d+) b=(\d+)", lines[-1]).groups())
assert r > 64000 and b < g < r, (r, g, b)
PY
echo '{"version":1,"enabled":false,"warmth":3000,"scheduled":false,"start":20,"end":7}' > "$XDG_CONFIG_HOME/helix/nightlight.json" 2>/dev/null || echo '{"version":1,"enabled":false}' > "$SCRATCH/config/helix/nightlight.json"
waitlog "NIGHTLIGHT neutral" 10 && echo "ok: switching it off lets the screen go back to normal" || fail "never went neutral"
for _ in $(seq 1 10); do grep -q "HELIX gamma output=.* reset" "$NL" && break; sleep 0.5; done
grep -q "HELIX gamma output=.* reset" "$NL" && echo "ok: and the compositor was told to restore the colours" || fail "the table was not released"
pgrep -f "helix-nightlight" >/dev/null && echo "ok: still running" || fail "it exited"
[ "$FAILED" = 0 ] && echo "PASS: nightlight" || exit 1
