#!/bin/bash
# The microphone and camera indicator in the bar: nothing shows while nothing records; a running capture stream shows the microphone, a running video
# stream the camera, and both go when the streams end. PipeWire is replaced by tools/nested/fake-pw-dump.sh (HELIX_PW_DUMP) and the camera devices by an
# empty folder, so only the scripted streams count.
#   HELIX_NESTED_SCRIPT=tools/nested/shell-privacy-test.sh tools/nested/run.sh privacy tools/nested/tauri-shell.sh HELIX_PW_DUMP=tools/nested/fake-pw-dump.sh HELIX_PRIVACY_DEV=/nonexistent
SCRATCH=$1; LOG=$SCRATCH/program.log; LAYOUT=$SCRATCH/state/layout.json; FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
shown() { python3 - "$LAYOUT" privacy <<'PY'
import json, sys
e = json.load(open(sys.argv[1]))["elements"].get(sys.argv[2])
sys.exit(0 if e and e["w"] > 0 else 1)
PY
}
wait_log() { for _ in $(seq 1 ${2:-80}); do grep -q "$1" "$LOG" && return 0; sleep 0.25; done; return 1; }
wait_state() { for _ in $(seq 1 40); do if shown; then [ "$1" = yes ] && return 0; else [ "$1" = no ] && return 0; fi; sleep 0.25; done; return 1; }
for _ in $(seq 1 120); do grep -q "READY shell" "$LOG" && break; sleep 0.25; done
S=$SCRATCH/state
wait_state no && echo "ok: nothing is shown while nothing records (a playing stream does not count)" || fail "indicator shown at start"
touch "$S/pw-step-1"
wait_log 'PRIVACY mic=\["Zoom"\] camera=\[\]' && echo "ok: the shell saw Zoom using the microphone" || fail "no microphone event: $(grep PRIVACY "$LOG")"
wait_state yes && echo "ok: the indicator appears" || fail "indicator not shown for the microphone: $(python3 -c "import json,sys; print(sorted(json.load(open(sys.argv[1]))[\"elements\"]))" "$LAYOUT")"
shot privacy-mic
touch "$S/pw-step-2"
wait_log 'PRIVACY mic=\["Zoom"\] camera=\["Cheese"\]' && echo "ok: and Cheese using the camera" || fail "no camera event"
sleep 1; shot privacy-both
touch "$S/pw-step-3"
wait_log 'PRIVACY mic=\[\] camera=\["Cheese"\]' && echo "ok: the microphone went when Zoom stopped" || fail "mic not cleared"
touch "$S/pw-step-4"
wait_log 'PRIVACY mic=\[\] camera=\[\]' && echo "ok: the camera went when Cheese stopped" || fail "camera not cleared"
wait_state no && echo "ok: the indicator is gone again" || fail "indicator stuck"
[ "$FAILED" = 0 ] && echo "PASS: privacy indicator" || exit 1
