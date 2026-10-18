#!/bin/bash
# Mobius Settings page (React): open it, go to Providers, type a LAN URL, press Save & test, and the routing file has it with Act still local.
#   tools/nested/ui-suite.sh models
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
for _ in $(seq 1 80); do [ -f "$SCRATCH/state/layout.json" ] && has_id open-models && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
echo "--- 1. open the page"
click_id open-models; sleep 1.5
for _ in $(seq 1 40); do has_id models-nav-providers && break; sleep 0.25; done
has_id models-nav-providers && has_id models-back && echo "ok: the Settings page opened" || { fail "page did not open"; shot models-noopen; }
shot models-overview
echo "--- 2. set a LAN URL and Save & test (the test fails, the address is kept)"
click_id models-nav-providers; sleep 0.8
for _ in $(seq 1 20); do has_id route-lan-url && break; sleep 0.25; done
click_id route-lan-url; wtype "http://127.0.0.1:9/v1"; sleep 0.4
shot models-lan
click_id route-probe-lan; sleep 2.5
shot models-saved
click_id models-back; sleep 1.2
for _ in $(seq 1 20); do has_id models-back || break; sleep 0.25; done
has_id models-back && fail "the page did not close" || echo "ok: Back closed the page"
F=$(find "$SCRATCH/state" -name inference-routing.toml 2>/dev/null | head -1)
[ -n "$F" ] && grep -q "127.0.0.1:9/v1" "$F" && echo "ok: the LAN URL was saved" || fail "URL not in routing file: $F"
awk '/\[profiles.act\]/{f=1} f&&/backend/{print; exit}' "$F" | grep -q '"local"' && echo "ok: Act stays local" || fail "act profile not local"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
