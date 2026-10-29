#!/bin/bash
# Settings > Default apps: what opens web links, mail and folders is read from the freedesktop lists and a choice is written to ~/.config/mimeapps.list
# without touching the rest of that file.
#   HELIX_NESTED_SCRIPT=tools/nested/settings-defaults-test.sh tools/nested/run.sh settings-defaults tools/nested/settings-defaults-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
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
LIST=$SCRATCH/config/mimeapps.list
click_id page-defaults; sleep 1.5
wait_id set-default:web-fox.desktop 20 && has_id set-default:web-owl.desktop && echo "ok: the browsers are offered, by name" || fail "no browser choices"
has_id set-default:web-ghost.desktop && fail "a hidden application is offered" || echo "ok: a hidden application is not"
has_id set-default:mail-postbox.desktop && has_id set-default:files-drawer.desktop && echo "ok: mail and files too" || fail "mail or files missing"
has_id set-default:video-fox.desktop && fail "a row for a kind nothing opens" || echo "ok: and no row for a kind nothing here opens"
shot defaults; sleep 1; shot defaults
click_id set-default:web-owl.desktop; sleep 1.5
grep -q "x-scheme-handler/https=owl.desktop;fox.desktop;" "$LIST" && echo "ok: the choice is first in mimeapps.list, the old one kept after it" || fail "list: $(cat "$LIST")"
grep -q "x-scheme-handler/http=owl.desktop;" "$LIST" && echo "ok: it was set for http too (a type Owl opens)" || fail "http: $(cat "$LIST")"
grep -q "text/html=owl.desktop" "$LIST" && fail "set for a type Owl does not open" || echo "ok: and not for html, which Owl does not open"
grep -q "# my own choices" "$LIST" && grep -q "text/plain=editor.desktop;" "$LIST" && grep -q "image/png=viewer.desktop;" "$LIST" && echo "ok: the rest of the file is as it was" || fail "file damaged: $(cat "$LIST")"
shot defaults-after; sleep 1; shot defaults-after
[ "$FAILED" = 0 ] && echo "PASS: settings defaults" || exit 1
