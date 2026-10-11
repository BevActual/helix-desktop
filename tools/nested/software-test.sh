#!/bin/bash
# Nested pointer smoke for Helix Software (fake catalog only).
#   HELIX_NESTED_SCRIPT=tools/nested/software-test.sh tools/nested/run.sh software tools/nested/software-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
for _ in $(seq 1 80); do has_id page-explore && break; sleep 0.25; done
sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
windows = json.loads(subprocess.check_output(["niri", "msg", "--json", "windows"]))
outputs = json.loads(subprocess.check_output(["niri", "msg", "--json", "outputs"]))
w = next((x for x in windows if "software" in (x.get("app_id") or "").lower() or "Software" in (x.get("title") or "")), None)
if w is None:
    raise SystemExit("no software window: " + str([(x.get("app_id"), x.get("title")) for x in windows]))
layout = w["layout"]
tile = layout.get("tile_pos_in_workspace_view") or [0, 0]
offset = layout.get("window_offset_in_tile") or [0, 0]
logical = next(iter(outputs.values()))["logical"]
print(f'{tile[0] + offset[0]} {tile[1] + offset[1]}|{logical["width"]} {logical["height"]}')
PY
) || { fail "could not locate window"; exit 1; }
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
echo "window at $UI_ORIGIN on a screen of $UI_SCREEN"
has_id page-explore && has_id page-library && has_id page-updates && has_id page-sources && echo "ok: pages" || fail "pages missing"
for _ in $(seq 1 40); do has_id "pkg:org.helix.Demo" && break; sleep 0.25; done
has_id "pkg:org.helix.Demo" && echo "ok: explore lists demo" || fail "demo missing"
click_id "action:org.helix.Demo"; sleep 1.2
for _ in $(seq 1 20); do has_id confirm-yes && break; sleep 0.25; done
has_id confirm-yes && echo "ok: install confirm shown" || fail "no confirm"
niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/software-confirm.png"
click_id confirm-yes; sleep 1
click_id page-library; sleep 1
for _ in $(seq 1 20); do has_id "pkg:org.helix.Demo" && break; sleep 0.25; done
has_id "pkg:org.helix.Demo" && echo "ok: demo is in library after fake install" || fail "not in library"
click_id page-updates; sleep 1
for _ in $(seq 1 20); do has_id "pkg:org.helix.Editor" && break; sleep 0.25; done
has_id "pkg:org.helix.Editor" && echo "ok: updates lists editor" || fail "updates missing"
click_id page-sources; sleep 1
for _ in $(seq 1 20); do has_id "source:flathub" && break; sleep 0.25; done
has_id "source:flathub" && echo "ok: sources listed" || fail "sources missing"
[ "$FAILED" = 0 ] && echo "PASS: software nested" || exit 1
