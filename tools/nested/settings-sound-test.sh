#!/bin/bash
# The Sound page of Settings against the fake desktop: every control is used by name and the exact Audio1 actions the service received
# are compared with what must (and must not) have been sent.
#   HELIX_NESTED_SCRIPT=tools/nested/settings-sound-test.sh tools/nested/run.sh settings-sound tools/nested/settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
AUDIO=$SCRATCH/state/fake-audio-actions.log
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
for _ in $(seq 1 120); do grep -q "SETTINGS audio" "$LOG" && break; sleep 0.25; done; sleep 2
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "settings" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
count() { [ -f "$AUDIO" ] && grep -c "^ACTION" "$AUDIO" || echo 0; }
click_id page-sound; sleep 1.5; shot settings-sound-open
echo "--- 1. the page lists the devices and sends nothing by being opened"
has_id "sound-out:Speakers" && has_id "sound-out:Headphones" && has_id "sound-in:Microphone" && echo "ok: outputs and inputs are listed" || fail "devices missing"
[ "$(count)" = 0 ] && echo "ok: nothing was sent" || fail "something was sent on open"
echo "--- 2. choosing the device already in use sends nothing; choosing another makes it the default"
N0=$(count); click_id "sound-out:Speakers"; sleep 0.5
[ $(( $(count) - N0 )) = 0 ] && echo "ok: the device in use is not re-selected" || fail "re-selecting the default sent something"
click_id "sound-out:Headphones"; sleep 1
echo "--- 3. mute and volume apply to the new default output"
click_id sound-output-mute; sleep 1
click_id set-sound-output-number; wtype -M ctrl -k a -m ctrl; wtype "30"; wtype -k Return; sleep 1
shot settings-sound-output
echo "--- 4. the microphone: unmute, then choose the other one"
click_id sound-input-mute; sleep 1
click_id "sound-in:Webcam Microphone"; sleep 1
shot settings-sound-input
EXPECTED='ACTION default {"id": 51}
ACTION mute {"id": 51, "value": true}
ACTION volume {"id": 51, "value": 0.3}
ACTION mute {"id": 70, "value": false}
ACTION default {"id": 71, "input": true}'
[ "$(grep '^ACTION' "$AUDIO")" = "$EXPECTED" ] && echo "ok: exactly the expected audio actions, in order" || { fail "audio actions differ"; cat "$AUDIO"; }
[ "$(grep -c '^REFUSED' "$AUDIO" 2>/dev/null)" = 0 ] && echo "ok: the service refused nothing" || fail "service refused something"
echo "--- 5. testing the microphone opens the one in use, shows a level, and closes it on leaving the page"
click_id sound-input-test; sleep 0.4
for _ in $(seq 1 40); do grep -q "METER start 71" "$AUDIO" && break; sleep 0.1; done
grep -q "METER start 71" "$AUDIO" && echo "ok: the meter started on the microphone in use" || { fail "meter did not start"; cat "$AUDIO"; }
for _ in $(seq 1 40); do has_id sound-input-heard && break; sleep 0.15; done
has_id sound-input-heard && echo "ok: the page shows it is hearing the microphone" || fail "no level on the page"
shot settings-sound-meter
click_id page-appearance; sleep 0.8
grep -q "METER stop" "$AUDIO" && echo "ok: leaving the page closed the microphone" || { fail "meter did not stop"; cat "$AUDIO"; }
[ "$(grep '^ACTION' "$AUDIO")" = "$EXPECTED" ] && echo "ok: the test sent no volume or mute changes" || fail "the meter changed the audio actions"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
