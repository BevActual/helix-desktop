#!/bin/bash
# The shell's own chrome follows Automatic by the time of day, not as "light always": with a schedule that makes it daytime the bar is light, with
# one that makes it night the bar is dark, and a fixed mode is unchanged. The bar is measured from a real screenshot.
#   HELIX_NESTED_SCRIPT=tools/nested/shell-auto-theme-test.sh tools/nested/run.sh shell-auto-theme tools/nested/tauri-fake-desktop.sh
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
update() { # id signature value (GVariant text)
  python3 - "$1" "$2" "$3" <<'PY'
import os, sys
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib
c = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
sig, raw = sys.argv[2], sys.argv[3]
value = raw == "true" if sig == "b" else raw
c.call_sync("org.helix.Desktop", "/org/helix/Desktop", "org.helix.Desktop.Configuration1", "Update",
            GLib.Variant("(sv)", (sys.argv[1], GLib.Variant(sig, value))), None, 0, 5000, None)
PY
}
# Mean brightness (0..255) of the top strip, where the bar is.
bar() {
  niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5
  python3 - "$(ls -t "$SCRATCH"/shots/*.png | head -1)" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("L")
w, h = im.size
band = im.crop((w // 4, 8, 3 * w // 4, 30))
px = list(band.getdata())
print(round(sum(px) / len(px)))
PY
}
settle() { # lt|gt limit: wait for the bar to repaint
  local v
  for _ in $(seq 1 10); do
    v=$(bar)
    if [ "$1" = lt ] && [ "$v" -lt "$2" ]; then echo "$v"; return 0; fi
    if [ "$1" = gt ] && [ "$v" -gt "$2" ]; then echo "$v"; return 0; fi
    sleep 0.5
  done
  echo "$v"; return 1
}
for _ in $(seq 1 120); do grep -q "LIVE Sound" "$LOG" && break; sleep 0.25; done; sleep 2
echo "--- 1. dark: the bar is dark"
update theme.mode s dark
DARK=$(settle lt 110) && echo "ok: dark bar ($DARK)" || fail "the dark bar is bright: $DARK"
echo "--- 2. automatic by day (a schedule that is always day): the bar is light"
update location.custom_schedule b true; update location.sunrise s 00:00; update location.sunset s 23:59
update theme.mode s auto
LIGHT=$(settle gt 150) && echo "ok: automatic by day is light ($LIGHT)" || fail "automatic by day is dark: $LIGHT"
echo "--- 3. automatic by night (a schedule that is always night): the bar is dark"
update location.sunset s 00:00; update location.sunrise s 23:59
NIGHT=$(settle lt 110) && echo "ok: automatic by night is dark ($NIGHT)" || fail "automatic by night is light: $NIGHT"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
