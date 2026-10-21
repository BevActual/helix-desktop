#!/bin/bash
# Settings > Date & time: the time zone and automatic time go to systemd-timedated (asking for authorisation), the clock choice is a file the
# shell watches, and the location and custom day and night times go to the desktop service.
#   HELIX_NESTED_SCRIPT=tools/nested/settings-datetime-test.sh tools/nested/run.sh settings-datetime tools/nested/settings-datetime-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0; S=$SCRATCH/state
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "settings" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
for _ in $(seq 1 120); do grep -q "SETTINGS bluetooth" "$LOG" && break; sleep 0.25; done; sleep 2
click_id page-datetime; sleep 1.5
wait_id set-time:zone 20 && echo "ok: the time zone is on the page" || fail "no time zone row"
wait_id set-time:ntp 5 && echo "ok: so is automatic time" || fail "no automatic time row"
has_id set-time:clock-24 && echo "ok: and the clock choice" || fail "no clock choice"
has_id set-place:auto && has_id set-place:latitude && has_id set-place:longitude && echo "ok: and where you are, by hand or automatically" || fail "no location rows"
has_id set-place:sunrise && fail "custom times shown while they are off" || echo "ok: custom times are hidden until asked for"
shot datetime; sleep 1; shot datetime
echo "--- the time zone, from the list"
click_id set-time:zone; wait_id set-time:zone-filter 10 && echo "ok: it opens a list with a filter" || fail "no list"
wtype "paris"; sleep 0.8
wait_id "set-time:zone-Europe/Paris" 10 && ! has_id "set-time:zone-Asia/Tokyo" && echo "ok: typing narrows the zones" || fail "filter did not narrow"
shot datetime-zones; sleep 1; shot datetime-zones
click_id "set-time:zone-Europe/Paris"; sleep 1.5
grep -q "SetTimezone Europe/Paris interactive=True" "$S/fake-timedate.log" && echo "ok: timedated was asked, with authorisation allowed" || fail "timezone call: $(cat "$S/fake-timedate.log" 2>&1)"
echo "--- automatic time"
click_id set-time:ntp; sleep 1.5
grep -q "SetNTP False interactive=True" "$S/fake-timedate.log" && echo "ok: automatic time off went to timedated" || fail "ntp call: $(cat "$S/fake-timedate.log")"
echo "--- the clock is a file the shell watches"
click_id set-time:clock-24; sleep 1.5
python3 -c 'import json,sys; sys.exit(0 if json.load(open(sys.argv[1]))["format"]=="24" else 1)' "$SCRATCH/config/helix/clock-settings.json" && echo "ok: 24-hour is kept in clock-settings.json" || fail "clock file: $(cat "$SCRATCH/config/helix/clock-settings.json" 2>&1)"
echo "--- finding the place automatically replaces the typed coordinates"
click_id set-place:auto; sleep 1.5
grep -q 'UPDATE location.auto_locate true' "$S/fake-settings-actions.log" && echo "ok: turning it on went to the desktop service" || fail "auto: $(cat "$S/fake-settings-actions.log")"
has_id set-place:latitude && fail "typed coordinates stay while it is automatic" || echo "ok: the typed coordinates go away while it finds the place itself"
shot datetime-auto; sleep 1; shot datetime-auto
click_id set-place:auto; sleep 1.5
grep -q 'UPDATE location.auto_locate false' "$S/fake-settings-actions.log" && has_id set-place:latitude && echo "ok: and come back when it is turned off" || fail "did not return to typed coordinates"
echo "--- location and custom times go to the desktop service"
click_id set-place:custom; sleep 1.5
grep -q 'UPDATE location.custom_schedule true' "$S/fake-settings-actions.log" && echo "ok: custom times on" || fail "custom: $(cat "$S/fake-settings-actions.log")"
wait_id set-place:sunset 10 && echo "ok: the day and night times appear" || fail "no sunset row"
click_id set-place:sunset; wait_id set-place:sunset-filter 10 && wtype "21:30"; sleep 0.8
click_id "set-place:sunset-21:30"; sleep 1.5
grep -q 'UPDATE location.sunset "21:30"' "$S/fake-settings-actions.log" && echo "ok: a night start time was saved" || fail "sunset: $(cat "$S/fake-settings-actions.log")"
grep -q REFUSED "$S/fake-settings-actions.log" && fail "the service refused something: $(grep REFUSED "$S/fake-settings-actions.log")" || echo "ok: nothing was refused"
shot datetime-after; sleep 1; shot datetime-after
[ "$FAILED" = 0 ] && echo "PASS: settings datetime" || exit 1
