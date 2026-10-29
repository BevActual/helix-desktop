#!/bin/bash
# A panel (here the launcher) opens on the screen that has the focus, not the one the shell started on. Two outputs; the panels surface is moved when a
# panel opens and the focused output is a different one.
#   tools/nested/ui-suite.sh shell-launcher-output
SCRATCH=$1; LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control; FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
for _ in $(seq 1 120); do grep -q "READY shell" "$LOG" && break; sleep 0.25; done; sleep 4
outputs() { niri msg --json outputs | python3 -c 'import json,sys; print(" ".join(sorted(json.load(sys.stdin))))'; }
panel_output() { niri msg --json layers | python3 -c '
import json,sys
for l in json.load(sys.stdin):
    if l.get("namespace") == "helix-panels": print(l.get("output")); break'; }
read -r A B <<< "$(outputs)"
[ -n "${B:-}" ] || fail "need two outputs, got: $(outputs)"
echo "outputs: $A $B; the panels surface starts on $(panel_output)"
for target in "$B" "$A" "$B"; do
  niri msg action focus-monitor "$target"; sleep 1
  echo launcher >> "$CONTROL"; sleep 3
  got=$(panel_output)
  [ "$got" = "$target" ] && echo "ok: the launcher opened on $target" || fail "focused $target but the launcher is on ${got:-nowhere}"
  # Close it before moving the focus again.
  echo close >> "$CONTROL"; sleep 2
done
[ "$FAILED" = 0 ] && echo "PASS: panels open on the focused screen" || exit 1
