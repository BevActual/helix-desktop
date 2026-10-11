#!/bin/bash
# Nested pointer smoke for Helix System Monitor (fake /proc).
#   HELIX_NESTED_SCRIPT=tools/nested/system-monitor-test.sh tools/nested/run.sh system-monitor tools/nested/system-monitor-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
for _ in $(seq 1 80); do has_id page-overview && break; sleep 0.25; done
sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
windows = json.loads(subprocess.check_output(["niri", "msg", "--json", "windows"]))
outputs = json.loads(subprocess.check_output(["niri", "msg", "--json", "outputs"]))
w = next((x for x in windows if "monitor" in (x.get("app_id") or "").lower() or "Monitor" in (x.get("title") or "")), None)
if w is None:
    raise SystemExit("no monitor window: " + str([(x.get("app_id"), x.get("title")) for x in windows]))
layout = w["layout"]
tile = layout.get("tile_pos_in_workspace_view") or [0, 0]
offset = layout.get("window_offset_in_tile") or [0, 0]
logical = next(iter(outputs.values()))["logical"]
print(f'{tile[0] + offset[0]} {tile[1] + offset[1]}|{logical["width"]} {logical["height"]}')
PY
) || { fail "could not locate window"; exit 1; }
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
echo "window at $UI_ORIGIN on a screen of $UI_SCREEN"
has_id page-overview && has_id page-processes && has_id "page-screen-time" && echo "ok: pages" || fail "pages missing"
for _ in $(seq 1 40); do has_id stat-cpu && break; sleep 0.25; done
has_id stat-cpu && has_id stat-mem && has_id stat-gpu && echo "ok: overview stats" || fail "overview missing"
for _ in $(seq 1 30); do grep -q "MONITOR gpu probe" "$SCRATCH/program.log" && break; sleep 0.5; done
grep -q "MONITOR gpu probe" "$SCRATCH/program.log" && echo "ok: gpu polled on overview" || fail "no gpu probe"
sleep 2.5; niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/monitor-overview.png"
click_id page-processes; sleep 1
for _ in $(seq 1 20); do has_id "proc:42" && break; sleep 0.25; done
has_id "proc:42" && echo "ok: process listed" || fail "process missing"
click_id page-screen-time; sleep 0.8
has_id "screen:org.helix.Settings" && echo "ok: screen time" || fail "screen time missing"
[ "$FAILED" = 0 ] && echo "PASS: system-monitor nested" || exit 1
