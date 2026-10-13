#!/bin/bash
# The volume and brightness keys through org.helix.Shell1 (what helix-panel calls): the backlight stand-in and a private PipeWire change by
# the right amount, never below the dimmest allowed, the indicator is shown, and the slider in the control center writes the backlight too.
#   HELIX_NESTED_SCRIPT=tools/nested/keys-test.sh tools/nested/run.sh keys tools/nested/tauri-shell.sh HELIX_NESTED_PRIVATE_AUDIO=1
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.."  && pwd); LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control; FAILED=0
export PIPEWIRE_RUNTIME_DIR=$SCRATCH/state/pipewire PIPEWIRE_REMOTE=pipewire-helix
BACKLIGHT=$SCRATCH/state/backlight/brightness
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
wait_for() { for _ in $(seq 1 "${3:-60}"); do [ "$(grep -c "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0; sleep 0.25; done; echo "TIMEOUT waiting for $2 x $1" >&2; return 1; }
call() { busctl --user call org.helix.Shell /org/helix/Shell org.helix.Shell1 "$@" >/dev/null; }
truth() { timeout 10 pw-dump 2>/dev/null | python3 -c "
import sys, json
for o in json.load(sys.stdin):
    props = o.get('info', {}).get('props', {})
    if props.get('node.name') == 'helix-test-sink-1':
        p = {}
        for entry in o.get('info', {}).get('params', {}).get('Props', []): p.update(entry)
        cv = p.get('channelVolumes') or [p.get('volume', 1.0)]
        print('%d %s' % (round(cv[0] ** (1/3) * 100), 'muted' if p.get('mute') else 'unmuted'))
        break"; }
wait_for "LIVE Sound" 1 120 || exit 1; sleep 2
echo "--- brightness keys"
[ "$(cat $BACKLIGHT)" = 50 ] || fail "the stand-in backlight did not start at 50"
call AdjustBrightness i 10; wait_for "OSD brightness 60" 1 20 || fail "no indicator for 60"
[ "$(cat $BACKLIGHT)" = 60 ] && echo "ok: 50 -> 60" || fail "backlight is $(cat $BACKLIGHT), wanted 60"
call AdjustBrightness i -- -100; wait_for "OSD brightness 5" 1 20 || fail "no indicator for the dimmest"
[ "$(cat $BACKLIGHT)" = 5 ] && echo "ok: never below 5" || fail "backlight is $(cat $BACKLIGHT), wanted 5"
echo "--- volume keys"
call AdjustVolume i -- -30; sleep 1.5
before=$(truth | cut -d' ' -f1)
call AdjustVolume i 5; wait_for "OSD volume" 1 20 || fail "no volume indicator"; sleep 1.2
after=$(truth | cut -d' ' -f1)
[ $((after - before)) -ge 4 ] && [ $((after - before)) -le 6 ] && echo "ok: volume $before -> $after" || fail "volume went $before -> $after"
call ToggleMute; wait_for "OSD volume .* muted" 1 20 || fail "no muted indicator"; sleep 1.2
[ "$(truth | cut -d' ' -f2)" = muted ] && echo "ok: muted" || fail "not muted"
call AdjustVolume i 5; sleep 1.5
[ "$(truth | cut -d' ' -f2)" = unmuted ] && echo "ok: volume up unmutes" || fail "volume up did not unmute"
echo "--- the slider"
echo control-center >> "$CONTROL"; wait_for "PANEL open" 1 20; sleep 1.5
for _ in $(seq 1 20); do has_id brightness && break; sleep 0.25; done
has_id brightness || fail "the brightness slider is missing"
python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" brightness 0.8 | sed "s/^/slider click at: /"; python3 -c "import json;print('slider box:', json.load(open('$LAYOUT'))['elements']['brightness'])"
click_id brightness 0.8; sleep 1
v=$(cat $BACKLIGHT); [ "$v" -ge 76 ] && [ "$v" -le 84 ] && echo "ok: slider set $v" || fail "slider left the backlight at $v"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
