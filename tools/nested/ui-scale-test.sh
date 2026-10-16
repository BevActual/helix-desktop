#!/bin/bash
# An application follows the interface scale from Settings > Accessibility: the window's content is drawn larger (the page has
# fewer CSS pixels across) when the scale goes up, and back at 1.0, with no restart.
#   HELIX_NESTED_SCRIPT=tools/nested/ui-scale-test.sh tools/nested/run.sh ui-scale tools/nested/system-monitor-scale-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
LAYOUT=$SCRATCH/state/layout.json
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
for _ in $(seq 1 80); do has_id page-overview && break; sleep 0.25; done
for _ in $(seq 1 40); do grep -q '^UI scale' "$LOG" && break; sleep 0.25; done
width() { python3 -c "import json,sys; print(round(json.load(open('$LAYOUT'))['width']))"; }
set_scale() {
  python3 - "$1" <<'PY'
import os, sys
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib
c = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
c.call_sync("org.helix.Desktop", "/org/helix/Desktop", "org.helix.Desktop.Configuration1", "Update",
            GLib.Variant("(sv)", ("accessibility.ui_scale", GLib.Variant("d", float(sys.argv[1])))), None, 0, 5000, None)
PY
}
wait_width() { for _ in $(seq 1 30); do [ "$(width)" = "$1" ] && return 0; sleep 0.25; done; return 1; }
sleep 1
BASE=$(width)
grep -q '^UI scale 1$' "$LOG" && echo "ok: the app read the scale at startup" || fail "no startup scale: $(grep 'UI scale' "$LOG")"
echo "--- 1. a larger scale: the page has fewer CSS pixels across"
set_scale 1.25
WANT=$(python3 -c "print(round($BASE/1.25))")
wait_width "$WANT" && echo "ok: $BASE px became $WANT px" || fail "width is $(width), expected $WANT (from $BASE)"
echo "--- 2. back to 1.0"
set_scale 1.0
wait_width "$BASE" && echo "ok: back to $BASE px" || fail "width is $(width), expected $BASE"
echo "--- 3. a value outside the bounds is kept within them"
set_scale 3.0
WANT=$(python3 -c "print(round($BASE/1.5))")
wait_width "$WANT" && echo "ok: 3.0 is drawn as 1.5" || fail "width is $(width), expected $WANT"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
