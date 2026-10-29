#!/bin/bash
# Mobius's hands are the actions catalogue (docs/design/actions.md): in Tools mode a (scripted) model finds a setting by what it does and
# changes it; the person approves the change in Mobius; the desktop applies it and records it in the one history with Mobius's name, and
# the person can undo it there.   tools/nested/ui-suite.sh mobius-actions
set -u
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
refresh_geometry() {
  local loc; loc=$(python3 - <<'PY'
import json,subprocess
w=next(x for x in json.loads(subprocess.check_output(['niri','msg','--json','windows'])) if 'mobius' in (x['app_id'] or '').lower())
o=next(iter(json.loads(subprocess.check_output(['niri','msg','--json','outputs'])).values()))['logical']
l=w['layout'];t=l.get('tile_pos_in_workspace_view') or [0,0];f=l.get('window_offset_in_tile') or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
  export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
}
click_id() { refresh_geometry; local pos; pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1" "${2:-0.5}"); "$PT" $pos click; sleep 0.8; }
wait_id() { for _ in $(seq 1 60); do has_id "$1" && return 0; sleep 0.25; done; echo "FAIL: missing $1"; exit 1; }
paste_id() { click_id "$1"; wl-copy --primary "$2"; refresh_geometry; local pos; pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1"); "$PT" $pos mclick; sleep 0.3; }
config() {
  python3 - "$@" <<'PY'
import os, sys, gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib
c = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
args = GLib.Variant.parse(None, sys.argv[2], None, None) if sys.argv[2] else None
out = c.call_sync("org.helix.Desktop", "/org/helix/Desktop", "org.helix.Desktop.Configuration1", sys.argv[1], args,
                  GLib.VariantType(sys.argv[3]), 0, 10000, None)
print(out.unpack()[0])
PY
}
value_of() { config GetSettings "" "(av)" | python3 -c "
import sys, ast
rows = ast.literal_eval(sys.stdin.read())
print(next((r.get('value') for r in rows if r.get('id') == '$1'), 'missing'))"; }

wait_id mode
BEFORE=$(value_of theme.pure_black_dark); echo "before: $BEFORE"
WANT=$([ "$BEFORE" = "True" ] && echo false || echo true)
printf '[{"tool":"settings_find","query":"pure black"},{"tool":"settings_set","setting":"theme.pure_black_dark","value":%s},{"tool":"finish"}]' "$WANT" > "$SCRATCH/state/tools-steps.json"
click_id mode
wait_id menu-mode-folder
click_id menu-mode-folder
wait_id tools-panel
click_id tools-enabled
click_id tools-choose
click_id tools-save
paste_id input 'Turn on pure black dark surfaces.'
click_id send
# The change is the person's to approve (reading settings is not); approve each step that asks, until the run ends.
for _ in $(seq 1 120); do
  has_id tool-approve && click_id tool-approve
  has_id stop || break
  sleep 0.5
done
sleep 1
python3 - "$SCRATCH" <<'PY' && echo "ok: Mobius found the setting and changed it (settings_find, settings_set, answer)" || fail "chat activities"
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
c = json.loads(next((p / 'data/helix/mobius/chats').glob('*.json')).read_text())
a = c['messages'][-1]['activities']
assert [x['tool'] for x in a] == ['settings_find', 'settings_set', 'answer'], [(x['tool'], x['state'], x.get('output', '')[:200]) for x in a]
assert all(x['state'] == 'completed' for x in a), [(x['tool'], x['state'], x.get('output', '')[:200]) for x in a]
assert 'theme.pure_black_dark' in a[0]['output'], a[0]['output'][:300]
assert 'Pure black dark surfaces' in a[1]['output'], a[1]['output'][:300]
PY
AFTER=$(value_of theme.pure_black_dark)
[ "$AFTER" != "$BEFORE" ] && echo "ok: the desktop has it ($AFTER)" || fail "value unchanged ($AFTER)"
H=$(config ActionHistory "(uint32 5,)" "(s)")
echo "$H" | python3 -c "
import sys, json
e = json.loads(sys.stdin.read())[0]
assert e['target'] == 'theme.pure_black_dark' and e['by'] == {'kind': 'agent', 'label': 'Mobius'}, e
print('ok: the history says', e['sentence'], 'by', e['by']['label'])" || fail "history: $H"
ENTRY=$(echo "$H" | python3 -c "import sys, json; print(json.loads(sys.stdin.read())[0]['id'])")
out=$(config UndoAction "(uint64 $ENTRY,)" "(s)")
[ "$(value_of theme.pure_black_dark)" = "$BEFORE" ] && echo "ok: the person undid it ($out)" || fail "undo: $out"
[ "$FAILED" = 0 ] && echo "PASS: mobius actions" || exit 1
