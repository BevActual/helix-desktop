#!/bin/bash
# Five attacks on a pending approval, from a program that is merely running as the person (no sandbox): a synthetic pointer click, synthetic
# keys, reading the screen, covering the dialog, and stealing the keyboard. docs/design/trusted-surface.md says what the compositor must do.
#   TRUSTED_EXPECT=stock       an unprotected compositor (stock niri, no trusted-surface block): the attacks on the compositor's side
#                              (pointer, capture, covering, focus) must all succeed, proving each is real; keys are already refused by the
#                              dialog itself (a key never allows)
#   TRUSTED_EXPECT=protected   the fork with the trusted-surface role (HELIX_NIRI_BIN, configured for the shell): every attack must fail
#   HELIX_NESTED_SCRIPT=tools/nested/trusted-attack-test.sh tools/nested/run.sh trusted-attack tools/nested/tauri-shell.sh
set -u
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log
export LAYOUT=$SCRATCH/state/layout-confirmer.json
EXPECT=${TRUSTED_EXPECT:-stock}; FAILED=0; CLIENT=""; ATTACKER=""
CAPTURE=$ROOT/rust/target/fast/wl-capture
[ -x "$CAPTURE" ] || CAPTURE=$ROOT/rust/target/release/wl-capture
source "$ROOT/tools/nested/ui-lib.sh"
cleanup() { for p in "$CLIENT" "$ATTACKER"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done; true; }
trap cleanup EXIT
count() { grep -c "$1" "$LOG" 2>/dev/null || true; }
wait_for() { for _ in $(seq 1 "${3:-80}"); do [ "$(count "$1")" -ge "$2" ] && return 0; sleep 0.25; done; echo "TIMEOUT waiting for $2 x $1" >&2; return 1; }
OPENED=0
open_dialog() {  # a pending approval, armed (its buttons work)
  OPENED=$((OPENED + 1)); local armed; armed=$(count "CONFIRMER armed"); rm -f "$LAYOUT"
  # A different program each time (a copy of the interpreter): the service knows a client by its executable, and one that already holds the
  # tiers asked for gets no dialog.
  mkdir -p "$SCRATCH/clients/a$OPENED"; cp "$(readlink -f "$(command -v python3)")" "$SCRATCH/clients/a$OPENED/python3"
  PYTHONHOME=$(python3 -c "import sys; print(sys.base_prefix)") "$SCRATCH/clients/a$OPENED/python3" "$ROOT/tools/nested/confirmer-request.py" observe act >"$SCRATCH/client.log" 2>&1 & CLIENT=$!
  wait_for "CONFIRMER open" $OPENED 40 && wait_for "CONFIRMER ready-replay" $OPENED 40 && wait_for "CONFIRMER armed" $((armed + 1)) 20 || exit 1
  for _ in $(seq 1 40); do has_id confirm-allow && break; sleep 0.25; done
}
close_dialog() {  # whatever happened, end it
  [ -n "$ATTACKER" ] && { kill "$ATTACKER" 2>/dev/null; wait "$ATTACKER" 2>/dev/null; ATTACKER=""; }
  if kill -0 "$CLIENT" 2>/dev/null; then wtype -k Escape; sleep 0.5; fi
  wait "$CLIENT" 2>/dev/null; CLIENT=""; sleep 0.5
}
approved() { grep -q 'COMPLETED ok=True' "$SCRATCH/client.log"; }
verdict() {  # name, succeeded(0/1)
  local what=BLOCKED want=0; [ "$2" = 1 ] && what=SUCCEEDED
  # On a stock compositor every attack but the keys succeeds; with the role every one fails.
  [ "$EXPECT" = stock ] && [ "$1" != "synthetic keys" ] && want=1
  if [ "$2" = "$want" ]; then echo "ok: $1: attack $what (expected on a $EXPECT compositor)"
  else echo "FAIL: $1: attack $what, not what a $EXPECT compositor should give" >&2; FAILED=1; fi
}
shot_pixel() {  # x y of the last niri screenshot, as r,g,b
  niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5
  python3 - "$(ls -t "$SCRATCH"/shots/*.png | head -1)" "$1" "$2" <<'PY'
import sys, gi
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file(sys.argv[1]); x, y = int(float(sys.argv[2])), int(float(sys.argv[3]))
n, rs, px = pb.get_n_channels(), pb.get_rowstride(), pb.get_pixels()
i = y * rs + x * n; print(px[i], px[i + 1], px[i + 2])
PY
}
box_center() { python3 -c "
import json
e = json.load(open('$LAYOUT'))['elements']['$1']; print(e['x'] + e['w'] / 2, e['y'] + e['h'] / 2)"; }

wait_for "LIVE Sound" 1 120 || exit 1
wait_for "CONFIRMER registered" 1 80 || exit 1
echo "expectation: $EXPECT"

echo "--- A1. a synthetic pointer click on Allow"
open_dialog; click_id confirm-allow; sleep 1.2
approved && verdict "synthetic click" 1 || verdict "synthetic click" 0
close_dialog

echo "--- A2. synthetic keys: n Tabs then Space, for n = 1, 2, 3 ... until Allow is pressed (each wrong guess is a visible Deny)"
KEYS_WORKED=0
for n in 1 2 3 4 5 6 7; do
  open_dialog
  for _ in $(seq 1 "$n"); do wtype -k Tab; sleep 0.15; done; wtype -k space; sleep 1
  if approved; then KEYS_WORKED=1; echo "approved with $n Tab(s)"; close_dialog; break; fi
  close_dialog
done
verdict "synthetic keys" "$KEYS_WORKED"

echo "--- A3. reading the screen through the screencopy protocol (the verified client's name and path)"
open_dialog
"$CAPTURE" "$SCRATCH/capture.ppm" | tee "$SCRATCH/capture.out"
case "$(python3 "$ROOT/tools/nested/capture_visible.py" "$SCRATCH/capture.ppm" "$LAYOUT" "$SCRATCH/capture.out" confirm-identity)" in
  VISIBLE*) verdict "screen capture" 1 ;;
  *) verdict "screen capture" 0 ;;
esac
close_dialog

echo "--- A4. covering the dialog with another Overlay surface"
open_dialog
python3 "$ROOT/tools/nested/attacker.py" cover >"$SCRATCH/attacker.out" 2>&1 & ATTACKER=$!
for _ in $(seq 1 40); do grep -q READY "$SCRATCH/attacker.out" && break; sleep 0.25; done
read -r cx cy <<<"$(box_center confirm-allow)"
pixel=$(shot_pixel "$cx" "$cy"); echo "pixel at Allow: $pixel"
case "$pixel" in 25[0-5]\ [0-9]\ [0-9]|25[0-5]\ [0-9]\ [0-9][0-9]) verdict "covering" 1 ;; *) verdict "covering" 0 ;; esac
close_dialog

echo "--- A5. stealing the keyboard with an exclusive-keyboard Overlay surface"
open_dialog
python3 "$ROOT/tools/nested/attacker.py" focus >"$SCRATCH/attacker.out" 2>&1 & ATTACKER=$!
for _ in $(seq 1 40); do grep -q READY "$SCRATCH/attacker.out" && break; sleep 0.25; done
sleep 1; wtype -k a; sleep 0.8
grep -q "KEY a" "$SCRATCH/attacker.out" && verdict "keyboard theft" 1 || verdict "keyboard theft" 0
close_dialog

[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
