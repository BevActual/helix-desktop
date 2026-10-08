#!/bin/bash
# Does the compositor blur behind the shell's glass? A striped window sits behind; panels are opened over it. Screenshots in
# /tmp/helix-nested/blur/named. Needs the fast or release build.
if [ -z "${HELIX_BLUR_INNER:-}" ]; then
  ROOT=$(cd "$(dirname "$0")/../.." && pwd)
  rm -rf /tmp/helix-nested/blur
  KDL='layer-rule {
    match namespace="^noctalia-panel$"
    background-effect {
        blur true
        xray false
        liquid-glass {
            refraction-strength 0.35
        }
    }
}
blur {
    passes 1
    offset 0.5
    noise 0.00
    saturation 1.0
}'
  HELIX_BLUR_INNER=1 HELIX_NESTED_KDL="$KDL" HELIX_SHELL_BIN="${HELIX_SHELL_BIN:-$ROOT/rust/target/fast/helix-shell-tauri}" HELIX_NESTED_SETTLE=8 \
    HELIX_NESTED_SCRIPT="$ROOT/tools/nested/blur-look.sh" "$ROOT/tools/nested/run.sh" blur "$ROOT/tools/nested/tauri-fake-desktop.sh" >/dev/null 2>&1
  ls /tmp/helix-nested/blur/named/; grep -h 'GLASS' /tmp/helix-nested/blur/program.log
  exit 0
fi
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control; OUT=$SCRATCH/named; mkdir -p "$OUT"
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.3; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$OUT/$1.png"; }
for _ in $(seq 1 120); do grep -q "LIVE Sound" "$LOG" && break; sleep 0.25; done
python3 "$ROOT/tools/nested/backdrop.py" & B=$!
sleep 3; shot 01-bar-over-stripes
echo control-center >> "$CONTROL"; sleep 1.8; shot 02-control-center
echo close >> "$CONTROL"; sleep 0.6
echo quick-insert >> "$CONTROL"; sleep 1.8; shot 03-quick-insert
echo close >> "$CONTROL"; sleep 0.5
kill $B
