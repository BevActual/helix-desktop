#!/bin/bash
# HELIX_NESTED_SCRIPT: the control center's sound controls, end to end against a private PipeWire (virtual sinks, no hardware).
#   HELIX_NESTED_SCRIPT=tools/nested/control-center-test.sh tools/nested/run.sh cc tools/nested/tauri-shell.sh HELIX_NESTED_PRIVATE_AUDIO=1
# The truth about volume and mute is read straight from PipeWire with pw-dump, not from the service the shell talks to.
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control
export PIPEWIRE_RUNTIME_DIR=$SCRATCH/state/pipewire PIPEWIRE_REMOTE=pipewire-helix
wait_for() { for _ in $(seq 1 "${3:-60}"); do [ "$(grep -c "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0; sleep 0.25; done; echo "TIMEOUT waiting for $2 x $1"; return 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
# volume (cubic root of channelVolumes is what the mixer reports) and mute of a node, from PipeWire itself
truth() { timeout 10 pw-dump 2>/dev/null | python3 -c "
import sys, json
for o in json.load(sys.stdin):
    props = o.get('info', {}).get('props', {})
    if props.get('node.name') == '$1':
        p = {}
        for entry in o.get('info', {}).get('params', {}).get('Props', []):
            p.update(entry)
        cv = p.get('channelVolumes') or [p.get('volume', 1.0)]
        print('%.3f %s' % (cv[0] ** (1/3), 'muted' if p.get('mute') else 'unmuted'))
        break
else:
    print('missing')"; }
source "$ROOT/tools/nested/ui-lib.sh"
wait_for "LIVE Sound" 1 120 || exit 1
sleep 2
echo "initial truth speakers: $(truth helix-test-sink-1)   headphones: $(truth helix-test-sink-2)"
echo control-center >> "$CONTROL"; wait_for "PANEL open" 1 && sleep 2; shot cc-open
echo "--- 1. clicking the slider at 70% sets the volume"
click_id volume 0.7; wait_for "AUDIO volume sent" 1 20 && sleep 1.2
echo "truth speakers after: $(truth helix-test-sink-1)   (the other sink must be unchanged: $(truth helix-test-sink-2))"
shot cc-volume
echo "--- 2. Mute mutes, and unmutes"
click_id mute; wait_for "AUDIO mute sent" 1 20 && sleep 1.2
echo "truth speakers: $(truth helix-test-sink-1)"; shot cc-muted
click_id mute; wait_for "AUDIO mute sent" 2 20 && sleep 1.2
echo "truth speakers after unmute: $(truth helix-test-sink-1)"
echo "--- 3. the output page: choosing the other output makes it the default"
click_id sound-open; click_id "out:Helix Test Headphones"; wait_for "AUDIO default sent" 1 20 && sleep 1.2; shot cc-default
echo "--- 4. Escape goes back, then closes the panel"
wtype -k Escape; sleep 1; wtype -k Escape; wait_for "PANEL closed" 1 20 && echo "ok: closed"
echo "--- audio actions sent: $(grep -c '^AUDIO ' "$LOG")"
