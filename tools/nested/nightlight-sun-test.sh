#!/bin/bash
# helix-nightlight following the sun: with the location the desktop service holds (here, a night that began three hours ago), the screen is warm; when
# the night is moved to the evening ahead, the service sees the new location within a second and lets the screen go back to normal.
#   HELIX_NESTED_SCRIPT=tools/nested/nightlight-sun-test.sh tools/nested/run.sh nightlight-sun tools/nested/nightlight-sun-nested.sh HELIX_NESTED_GAMMA=1
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; NL=$SCRATCH/niri.log; FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
waitlog() { for _ in $(seq 1 "${2:-60}"); do grep -q "$1" "$LOG" 2>/dev/null && return 0; sleep 0.5; done; return 1; }
waitlog "NIGHTLIGHT settings enabled=true warmth=3000 scheduled=true" 40 && echo "ok: it read the settings, following the sun" || fail "settings not read: $(tail -3 "$LOG")"
waitlog "NIGHTLIGHT warm" 20 && echo "ok: it is night at the place the desktop reports, so the screen goes warm" || fail "never went warm: $(tail -5 "$LOG")"
for _ in $(seq 1 20); do grep -q "HELIX gamma output=.* r=" "$NL" && break; sleep 0.5; done
python3 - "$NL" <<'PY' && echo "ok: with a warm table (less green, least blue)" || fail "no warm table"
import re, sys
lines = [l for l in open(sys.argv[1]) if "HELIX gamma output=" in l and " r=" in l]
r, g, b = (int(x) for x in re.search(r"r=(\d+) g=(\d+) b=(\d+)", lines[-1]).groups())
assert r > 64000 and b < g < r, (r, g, b)
PY
echo "--- the night is moved to the evening ahead"
python3 - <<'PY'
import datetime as d
from gi.repository import Gio, GLib
n = d.datetime.now()
f = lambda t: t.strftime("%H:%M")
conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
for key, value in (("location.sunset", f(n + d.timedelta(hours=3))), ("location.sunrise", f(n + d.timedelta(hours=9)))):
    conn.call_sync("org.helix.Desktop", "/org/helix/Desktop", "org.helix.Desktop.Configuration1", "Update", GLib.Variant("(sv)", (key, GLib.Variant("s", value))), None, Gio.DBusCallFlags.NONE, 5000, None)
PY
waitlog "NIGHTLIGHT neutral" 30 && echo "ok: in daylight again, it let go of the screen" || fail "never went neutral: $(grep NIGHTLIGHT "$LOG" | tail -4)"
[ "$FAILED" = 0 ] && echo "PASS: nightlight sun" || exit 1
