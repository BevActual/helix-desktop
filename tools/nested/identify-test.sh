#!/bin/bash
# helix-identify puts a badge on each named screen, nowhere else, and takes itself away on time. Two outputs.
#   tools/nested/ui-suite.sh identify
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
BIN=$(ls -t "$ROOT/target/fast/helix-identify" "$ROOT/target/release/helix-identify" 2>/dev/null | head -1)
sleep 3
read -r A B <<< "$(niri msg --json outputs | python3 -c 'import json,sys; print(" ".join(sorted(json.load(sys.stdin))))')"
[ -n "${B:-}" ] || fail "need two outputs"
badges() { niri msg --json layers | python3 -c '
import json,sys
print(" ".join(sorted(l["output"] for l in json.load(sys.stdin) if l.get("namespace") == "helix-identify")))'; }
"$BIN" "$A=1" "$B=2" --seconds 6 &
sleep 2
[ "$(badges)" = "$A $B" ] && echo "ok: a badge on each screen" || fail "badges on: $(badges)"
niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/identify.png"
sleep 6
[ -z "$(badges)" ] && echo "ok: the badges left on time" || fail "still there: $(badges)"
"$BIN" "$A=1" "nonexistent=9" --seconds 4 &
sleep 2
[ "$(badges)" = "$A" ] && echo "ok: only the named screen that exists gets one" || fail "badges on: $(badges)"
sleep 4
"$BIN" 2>/dev/null; [ $? = 2 ] && echo "ok: no names is a usage error" || fail "no names accepted"
[ "$FAILED" = 0 ] && echo "PASS: identify" || exit 1
