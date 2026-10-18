#!/bin/bash
# The composer's + menu (files, a picture, an image, online sources), and the chat list's rows.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-composer-test.sh tools/nested/run.sh mobius-composer tools/nested/mobius-nested.sh <wide window>
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
CHATS=$SCRATCH/data/helix/mobius/chats
for _ in $(seq 1 80); do has_id input && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri", "msg", "--json", "windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri", "msg", "--json", "outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0, 0]; f = l.get("window_offset_in_tile") or [0, 0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
gone_id() { for _ in $(seq 1 24); do has_id "$1" || return 0; sleep 0.25; done; return 1; }
idle() { sleep 2; for _ in $(seq 1 120); do ! has_id stop && break; sleep 0.5; done; sleep 1.5; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
wait_id plus && echo "ok: the composer has a + button" || fail "no + button"
has_id attach && fail "the separate attach button is still showing" || echo "ok: attach and picture are not separate buttons"
click_id plus
wait_id menu-plus-files && wait_id menu-plus-picture && wait_id menu-plus-image && wait_id menu-plus-web && echo "ok: the menu offers files, a picture, an image and sources" || fail "menu incomplete"
shot plus; sleep 1; shot plus
wtype -k Escape; sleep 1
has_id menu-plus-files && fail "Escape did not close the menu" || echo "ok: Escape closed it"
click_id plus; click_id menu-plus-image
wait_id create-image-panel && echo "ok: Create an image opened its panel" || fail "no image panel"
click_id create-image-close; sleep 1
echo "--- the chat list"
click_id input; wtype "First question about cron"; wtype -k Return; idle
click_id new-chat; sleep 1.5; click_id input; wtype "Second question about systemd"; wtype -k Return; idle
wait_id chat-1 && echo "ok: two chats are listed" || fail "list has fewer than two chats"
shot list; sleep 1; shot list
[ "$FAILED" = 0 ] && echo "PASS: mobius composer" || exit 1
