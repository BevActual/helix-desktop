#!/bin/bash
# See the shell: every panel and state screenshotted in a nested session against the fake desktop, for design iteration.
#   tools/nested/look.sh            (builds nothing: uses the fast binary, run tools/iterate.sh or cargo build --profile fast first)
# Screenshots land in /tmp/helix-nested/look/shots-named/<name>.png
if [ -z "${HELIX_LOOK_INNER:-}" ]; then
  ROOT=$(cd "$(dirname "$0")/../.." && pwd)
  rm -rf /tmp/helix-nested/look/named
  HELIX_LOOK_INNER=1 HELIX_SHELL_BIN="${HELIX_SHELL_BIN:-$ROOT/rust/target/fast/helix-shell-tauri}" HELIX_NESTED_SETTLE=8 \
    HELIX_NESTED_SCRIPT="$ROOT/tools/nested/look.sh" "$ROOT/tools/nested/run.sh" look "$ROOT/tools/nested/tauri-fake-desktop.sh" >/dev/null 2>&1
  ls /tmp/helix-nested/look/named/
  exit 0
fi
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control; OUT=$SCRATCH/named; mkdir -p "$OUT"
wait_for() { for _ in $(seq 1 "${3:-60}"); do [ "$(grep -c "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0; sleep 0.25; done; return 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.3; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$OUT/$1.png"; }
source "$ROOT/tools/nested/ui-lib.sh"
probe() { python3 "$ROOT/tools/nested/notify-probe.py" "$@" >/dev/null; }
wait_for "LIVE Sound" 1 120; sleep 1
export MPRIS_LOG=$SCRATCH/mpris.log MPRIS_CMD=$SCRATCH/mpris.cmd; : > "$MPRIS_CMD"
python3 "$ROOT/tools/nested/mpris-app.py" 2>/dev/null & PLAYER=$!
trap 'kill $PLAYER 2>/dev/null' EXIT
sleep 2
shot 01-bar
PT="$ROOT/tools/nested/wl-pointer/wl-pointer"
"$PT" 0.5 0.999; sleep 1.2; shot 06-dock
"$PT" 0.47 0.965; sleep 0.9; shot 07-dock-hover
"$PT" 0.5 0.4; sleep 1
probe send "Messages" "Alex Rivera" "Are we still on for 6? I booked the table by the window." 1 -1 reply Reply
probe send "Calendar" "Design review in 10 minutes" "Studio B · Bring the latest prototype" 1 -1
sleep 1; shot 02-toasts
sleep 8
probe send "Power" "Battery is low" "12% remaining. Plug in soon." 2 -1
for panel in control-center launcher assistant notifications; do
  echo "$panel" >> "$CONTROL"; sleep 1.6; shot "10-$panel"
  echo close >> "$CONTROL"; sleep 0.8
done
busctl --user call org.helix.Shell /org/helix/Shell org.helix.Shell1 AdjustBrightness i 15 >/dev/null; sleep 0.6; shot 05-osd
sleep 2
wl-copy "Meeting notes: ship the shell polish pass before Friday." ; sleep 0.4; wl-copy "https://helix.example/docs/quick-insert"; sleep 0.6
echo quick-insert >> "$CONTROL"; sleep 1.6; shot 30-quick-emoji
wtype "smile"; sleep 0.8; shot 31-quick-emoji-search
wtype -k Tab; sleep 0.8; shot 32-quick-clipboard
wtype -k Tab; sleep 0.8; shot 33-quick-actions
echo close >> "$CONTROL"; sleep 0.8
echo control-center >> "$CONTROL"; sleep 1.2
for page in tile-wifi-open tile-bt-open sound-open; do
  click_id "$page" && sleep 0.9; shot "20-detail-$page"; click_id back; sleep 0.5
done
