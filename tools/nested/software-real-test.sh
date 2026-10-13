#!/bin/bash
# The real catalog: Flatpak's caches are read, the library shows what is installed, icons come from the appstream data.
# Skipped where Flatpak has no installed apps.
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { rm -f "$SCRATCH"/shots/*.png; niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
REAL_HOME=$(getent passwd "$(id -u)" | cut -d: -f6)
APP=$(HOME=$REAL_HOME XDG_DATA_HOME=$REAL_HOME/.local/share flatpak list --app --columns=application 2>/dev/null | head -1)
[ -n "$APP" ] || { echo "skip: no Flatpak apps installed here"; exit 0; }
for _ in $(seq 1 80); do has_id page-library && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "software" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
click_id page-library; sleep 1
for _ in $(seq 1 80); do has_id "pkg:$APP" && break; sleep 0.25; done
has_id "pkg:$APP" && echo "ok: an installed application is in the real library" || fail "installed app $APP missing"
shot software-real-library
click_id page-discover; sleep 1
for _ in $(seq 1 120); do has_id hero && break; sleep 0.5; done
has_id hero && echo "ok: Discover has a featured app from the real appstream index" || fail "no hero (index not built?)"
shot software-real-discover
click_id "cat-Game"; sleep 2
for _ in $(seq 1 40); do has_id count && break; sleep 0.25; done
has_id count && echo "ok: a category page lists real apps" || fail "no category list"
shot software-real-category
[ $FAILED = 0 ] && echo "PASS: software real" || exit 1
