#!/bin/bash
# Memory and time of the Settings Dock page: opens it, waits, and prints what the app's processes hold. Used to compare page builds.
#   HELIX_SETTINGS_BIN=<binary> HELIX_NESTED_SCRIPT=tools/nested/measure-settings-test.sh tools/nested/run.sh measure tools/nested/settings-fake-nested.sh
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log
source "$ROOT/tools/nested/ui-lib.sh"
for _ in $(seq 1 120); do grep -q "SETTINGS bluetooth" "$LOG" && break; sleep 0.25; done; sleep 2
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "settings" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
rss() {  # total KiB of the processes started with this run's state directory
  local total=0 p
  for p in $(pgrep -f "WebKit|helix-settings"); do
    tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -q "^XDG_STATE_HOME=$SCRATCH/state$" || continue
    total=$((total + $(awk '/^Pss:/{s+=$2} END{print s+0}' /proc/$p/smaps_rollup 2>/dev/null)))
  done
  echo $total
}
echo "settings open, Appearance page: $(rss) KiB (PSS)"
t0=$(date +%s.%N)
click_id page-dock
for _ in $(seq 1 100); do has_id set-auto_hide && break; done
t1=$(date +%s.%N)
echo "Dock page shown after $(python3 -c "print(round(($t1-$t0)*1000))") ms (includes the click and the wait for the layout report)"
sleep 3
echo "settings open, Dock page: $(rss) KiB (PSS)"
has_id react-page && echo "page: React" || echo "page: hand-written"
