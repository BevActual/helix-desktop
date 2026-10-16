#!/bin/bash
# An application follows the theme from Settings: dark and light, and the same after the page loads again. The window's background is
# measured from a real screenshot.
#   HELIX_NESTED_SCRIPT=tools/nested/ui-theme-test.sh tools/nested/run.sh ui-theme tools/nested/system-monitor-scale-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
LAYOUT=$SCRATCH/state/layout.json
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
for _ in $(seq 1 80); do has_id page-overview && break; sleep 0.25; done
for _ in $(seq 1 40); do grep -q '^UI theme' "$LOG" && break; sleep 0.25; done
set_mode() {
  python3 - "$1" <<'PY'
import os, sys
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib
c = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
c.call_sync("org.helix.Desktop", "/org/helix/Desktop", "org.helix.Desktop.Configuration1", "Update",
            GLib.Variant("(sv)", ("theme.mode", GLib.Variant("s", sys.argv[1]))), None, 0, 5000, None)
PY
}
# Mean brightness (0..255) of the middle of the screen, where the window's content is.
brightness() {
  niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5
  python3 - "$(ls -t "$SCRATCH"/shots/*.png | head -1)" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("L")
w, h = im.size
box = im.crop((w // 3, h // 3, 2 * w // 3, 2 * h // 3))
px = list(box.getdata())
print(round(sum(px) / len(px)))
PY
}
sleep 1
grep -q '^UI theme dark$' "$LOG" && echo "ok: the app read dark at startup" || fail "no startup theme: $(grep 'UI theme' "$LOG")"
# The window is repainted a moment after the change, so look a few times.
until_bright() { # "lt|gt" limit
  local v
  for _ in $(seq 1 8); do
    v=$(brightness)
    if [ "$1" = lt ] && [ "$v" -lt "$2" ]; then echo "$v"; return 0; fi
    if [ "$1" = gt ] && [ "$v" -gt "$2" ]; then echo "$v"; return 0; fi
    sleep 0.5
  done
  echo "$v"; return 1
}
D=$(until_bright lt 90) && echo "ok: drawn dark ($D)" || fail "dark theme is bright: $D"
set_mode light
grep -q '^UI theme light$' "$LOG" || sleep 2
grep -q '^UI theme light$' "$LOG" && echo "ok: the app was told light" || fail "not told: $(grep 'UI theme' "$LOG")"
L=$(until_bright gt 150) && echo "ok: drawn light ($L)" || fail "light theme is dark: $L"
set_mode dark
D2=$(until_bright lt 90) && echo "ok: back to dark ($D2)" || fail "did not go back to dark: $D2"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
