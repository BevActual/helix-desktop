#!/usr/bin/env bash
# The desktop service's automatic location against a scripted GeoClue on a private "system" bus: turning location.auto_locate on asks GeoClue (for
# this program, at city accuracy), the answer is kept rounded in location.latitude, location.longitude and location.address, the status file says
# so, and turning it off stops the client. A GeoClue that refuses is reported as denied, and nothing is changed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${HELIX_SERVICE_BIN:-$ROOT/target/fast/helix-desktop}"
export WAYLAND_DISPLAY="wayland-location-$$"

if [[ -z "${HELIX_DESKTOP_LOCATION_INNER:-}" ]]; then
  cargo build --profile fast -p helix-desktop --quiet
  exec dbus-run-session -- env HELIX_DESKTOP_LOCATION_INNER=1 HELIX_SERVICE_BIN="$BIN" "$0"
fi

XDG_CONFIG_HOME="$(mktemp -d)"; XDG_STATE_HOME="$(mktemp -d)"; export XDG_CONFIG_HOME XDG_STATE_HOME
export HELIX_ASSETS_DIR="$ROOT/assets"
unset HELIX_CONFIG_HOME HELIX_STATE_HOME
mkdir -p "$XDG_CONFIG_HOME/helix"
BUS=$(dbus-daemon --config-file="$ROOT/tools/nested/bus.conf" --fork --print-address --print-pid 2>/dev/null)
ADDRESS=$(echo "$BUS" | sed -n 1p); BUSPID=$(echo "$BUS" | sed -n 2p)
export DBUS_SYSTEM_BUS_ADDRESS="$ADDRESS"
PIDS=("$BUSPID")
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT
start_geoclue() {  # $1: extra env
  : > "$XDG_STATE_HOME/fake-geoclue.out"
  env $1 python3 "$ROOT/tools/nested/fake-geoclue.py" "$XDG_STATE_HOME" "$ADDRESS" > "$XDG_STATE_HOME/fake-geoclue.out" 2>&1 &
  GEO=$!; PIDS+=("$GEO")
  for _ in $(seq 1 50); do grep -q ready "$XDG_STATE_HOME/fake-geoclue.out" && break; sleep 0.1; done
}
start_geoclue "X=1"
"$BIN" > "$XDG_STATE_HOME/service.log" 2>&1 &
PIDS+=("$!")
for _ in $(seq 1 60); do
  gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.NameHasOwner org.helix.Desktop 2>/dev/null | grep -q true && break
  sleep 0.1
done

python3 - "$XDG_STATE_HOME" <<'PY'
import json, os, sys, time
from gi.repository import Gio, GLib
state = sys.argv[1]
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
def call(method, params=None, reply=None):
    return bus.call_sync("org.helix.Desktop", "/org/helix/Desktop", "org.helix.Desktop.Configuration1", method, params, reply, Gio.DBusCallFlags.NONE, 5000, None)
call("RegisterDesktop")
b = GLib.VariantBuilder.new(GLib.VariantType("a{sv}"))
b.add_value(GLib.Variant.new_dict_entry(GLib.Variant.new_string("gamma-control"), GLib.Variant.new_variant(GLib.Variant.new_boolean(True))))
p = GLib.VariantBuilder.new(GLib.VariantType("(a{sv})")); p.add_value(b.end())
call("RegisterCapabilities", p.end())
def settings():
    out = {}
    for entry in call("GetSettings", None, GLib.VariantType("(av)")).unpack()[0]:
        e = entry if isinstance(entry, dict) else entry
        out[e["id"]] = e.get("value")
    return out
def update(key, value):
    v = GLib.Variant("b", value) if isinstance(value, bool) else GLib.Variant("s", value)
    call("Update", GLib.Variant("(sv)", (key, v)))
def status():
    try: return json.load(open(os.path.join(state, "helix/location-status.json")))
    except Exception: return {}
def log():
    try: return open(os.path.join(state, "fake-geoclue.log")).read()
    except Exception: return ""
def wait(check, seconds=15):
    end = time.time() + seconds
    while time.time() < end:
        if check(): return True
        time.sleep(0.2)
    return False

before = settings()
assert before["location.auto_locate"] is False, before["location.auto_locate"]
print("ok: automatic location starts off, and nothing asked GeoClue", flush=True)
assert "GetClient" not in log(), log()
update("location.auto_locate", True)
assert wait(lambda: abs(settings()["location.latitude"] - 48.86) < 1e-6), (settings()["location.latitude"], log())
s = settings()
assert s["location.longitude"] == 2.35 and s["location.address"] == "Paris, France", s
print("ok: turning it on asked GeoClue and kept the answer, rounded to the hundredth (48.86, 2.35, Paris, France)", flush=True)
assert "Start desktop_id=org.helix.Desktop accuracy=4 threshold=50000" in log(), log()
print("ok: as org.helix.Desktop, at city accuracy, moving 50 km before it says again", flush=True)
assert status().get("state") == "located", status()
print("ok: the status file says located:", status()["message"], flush=True)
update("location.auto_locate", False)
assert wait(lambda: "Stop" in log()), log()
assert wait(lambda: status().get("state") == "off")
print("ok: turning it off stopped the client", flush=True)
PY
echo "--- GeoClue refuses"
kill "$GEO" 2>/dev/null || true; sleep 0.5
start_geoclue "HELIX_FAKE_GEOCLUE=deny"
python3 - "$XDG_STATE_HOME" <<'PY'
import json, os, sys, time
from gi.repository import Gio, GLib
state = sys.argv[1]
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
def call(method, params=None, reply=None):
    return bus.call_sync("org.helix.Desktop", "/org/helix/Desktop", "org.helix.Desktop.Configuration1", method, params, reply, Gio.DBusCallFlags.NONE, 5000, None)
call("Update", GLib.Variant("(sv)", ("location.auto_locate", GLib.Variant("b", True))))
end = time.time() + 15; st = {}
while time.time() < end:
    try: st = json.load(open(os.path.join(state, "helix/location-status.json")))
    except Exception: st = {}
    if st.get("state") == "denied": break
    time.sleep(0.2)
assert st.get("state") == "denied" and "geoclue" in st.get("message", "").lower(), st
print("ok: a refusal is reported as denied, in words:", st["message"], flush=True)
PY
echo "PASS: helix-desktop location"
