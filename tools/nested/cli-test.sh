#!/bin/bash
# `helix`, the command line for Helix's actions (docs/design/north-star.md, "open by design"), against the real desktop service: list the
# catalogue, find a setting by what it does, read it, change it (the sentence and how to undo), the history under "helix command line",
# undo it, and a setting that runs a program refused with a non-zero exit. The binary is pre-granted observe and act, as if the person had
# allowed it once.   tools/nested/ui-suite.sh cli
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
H=$ROOT/target/fast/helix
fail() { echo "FAIL: $*" >&2; FAILED=1; }
for _ in $(seq 1 120); do busctl --user status org.helix.Desktop >/dev/null 2>&1 && break; sleep 0.25; done; sleep 3

out=$("$H" actions) && echo "$out" | grep -q "settings.set" && echo "ok: helix actions lists the catalogue" || fail "actions: $out"
out=$("$H" find pure black) && echo "$out" | grep -q "^theme.pure_black_dark" && echo "ok: helix find pure black → $(echo "$out" | head -1)" || fail "find: $out"
BEFORE=$("$H" --json get theme.pure_black_dark | python3 -c "import sys, json; print(json.load(sys.stdin)['value'])")
echo "before: $BEFORE"
WANT=$([ "$BEFORE" = "True" ] && echo false || echo true)
out=$("$H" set theme.pure_black_dark "$WANT") && echo "$out" | grep -q "^Pure black dark surfaces:" && echo "ok: helix set says what changed: $(echo "$out" | head -1)" || fail "set: $out"
ENTRY=$(echo "$out" | sed -n 's/^Undo with: helix undo //p')
[ -n "$ENTRY" ] && echo "ok: and how to undo it (entry $ENTRY)" || fail "no undo hint"
out=$("$H" history -n 3) && echo "$out" | head -1 | grep -q "helix command line" && echo "ok: helix history: $(echo "$out" | head -1 | tr -s ' ')" || fail "history: $out"
out=$("$H" undo "$ENTRY") && echo "$out" | grep -q "^Undid Pure black" && echo "ok: helix undo: $out" || fail "undo: $out"
AFTER=$("$H" --json get theme.pure_black_dark | python3 -c "import sys, json; print(json.load(sys.stdin)['value'])")
[ "$AFTER" = "$BEFORE" ] && echo "ok: the value is back ($AFTER)" || fail "not restored: $AFTER"
if "$H" set hooks.started "touch /tmp/owned" 2>"$SCRATCH/blocked.err"; then fail "a hook was set"; else grep -q "may not change this" "$SCRATCH/blocked.err" && echo "ok: a setting that runs a program is refused: $(cat "$SCRATCH/blocked.err")" || fail "refusal: $(cat "$SCRATCH/blocked.err")"; fi
out=$("$H" do settings.find query=dock) && echo "$out" | grep -q "dock" && echo "ok: helix do runs any action with key=value parameters" || fail "do: $out"
[ "$FAILED" = 0 ] && echo "PASS: cli" || exit 1
