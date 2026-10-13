#!/usr/bin/env bash
# Private-bus smoke for Rust helix-desktop Configuration1 catalog + runtime state.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${HELIX_SERVICE_BIN:-$ROOT/target/debug/helix-desktop}"
export XDG_CONFIG_HOME XDG_STATE_HOME
XDG_CONFIG_HOME="$(mktemp -d)"
XDG_STATE_HOME="$(mktemp -d)"
"$BIN" &
PID=$!
trap 'kill "$PID" 2>/dev/null || true' EXIT
for _ in $(seq 1 40); do
  gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
    --method org.freedesktop.DBus.NameHasOwner org.helix.Desktop 2>/dev/null | grep -q true && break
  sleep 0.05
done

python3 - <<'PY'
import re
import subprocess
from gi.repository import Gio, GLib

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

def call(method, params=None, reply=GLib.VariantType("(a{sv})")):
    return bus.call_sync(
        "org.helix.Desktop",
        "/org/helix/Desktop",
        "org.helix.Desktop.Configuration1",
        method,
        params,
        reply,
        Gio.DBusCallFlags.NONE,
        -1,
        None,
    )

# Catalog
raw = subprocess.check_output([
    "gdbus", "call", "--session", "--dest", "org.helix.Desktop",
    "--object-path", "/org/helix/Desktop",
    "--method", "org.helix.Desktop.Configuration1.GetSettings",
], text=True)
ids = re.findall(r"'id': <'([^']+)'>", raw)
print("setting_count=", len(ids))
assert len(ids) >= 40, ids
for want in [
    "nightlight.force", "nightlight.temperature_day", "theme.builtin", "wallpaper.enabled",
    "shell.offline_mode", "notification.enable_daemon", "control_center.width", "osd.kinds.volume",
]:
    assert want in ids, want
print("catalog=OK")

# One connection owns RegisterDesktop → capabilities → state
call("RegisterDesktop", None, None)
builder = GLib.VariantBuilder.new(GLib.VariantType("a{sv}"))
builder.add_value(
    GLib.Variant.new_dict_entry(
        GLib.Variant.new_string("gamma-control"),
        GLib.Variant.new_variant(GLib.Variant.new_boolean(True)),
    )
)
caps = builder.end()
params = GLib.VariantBuilder.new(GLib.VariantType("(a{sv})"))
params.add_value(caps)
call("RegisterCapabilities", params.end(), None)

snap = call("GetSnapshot")
fields = dict(snap.unpack()[0])
assert fields["helix-desktop"] is True
caps_out = dict(fields["desktop-capabilities"])
assert caps_out["gamma-control"] is True
print("capabilities=OK")

params = GLib.VariantBuilder.new(GLib.VariantType("(ssv)"))
params.add_value(GLib.Variant.new_string("shell"))
params.add_value(GLib.Variant.new_string("panel-open"))
params.add_value(GLib.Variant.new_variant(GLib.Variant.new_boolean(True)))
call("SetState", params.end(), None)
params = GLib.VariantBuilder.new(GLib.VariantType("(sss)"))
params.add_value(GLib.Variant.new_string("shell"))
params.add_value(GLib.Variant.new_string("panel-open"))
params.add_value(GLib.Variant.new_string("boolean"))
state = dict(call("GetState", params.end()).unpack()[0])
assert state["present"] is True and state["value"] is True

params = GLib.VariantBuilder.new(GLib.VariantType("(s)"))
params.add_value(GLib.Variant.new_string("shell"))
call("ClearState", params.end(), None)
params = GLib.VariantBuilder.new(GLib.VariantType("(sss)"))
params.add_value(GLib.Variant.new_string("shell"))
params.add_value(GLib.Variant.new_string("panel-open"))
params.add_value(GLib.Variant.new_string("boolean"))
state = dict(call("GetState", params.end()).unpack()[0])
assert state["present"] is False
print("runtime_state=OK")

params = GLib.VariantBuilder.new(GLib.VariantType("(sv)"))
params.add_value(GLib.Variant.new_string("theme.builtin"))
params.add_value(GLib.Variant.new_variant(GLib.Variant.new_string("Catppuccin")))
call("Update", params.end(), None)
raw = subprocess.check_output([
    "gdbus", "call", "--session", "--dest", "org.helix.Desktop",
    "--object-path", "/org/helix/Desktop",
    "--method", "org.helix.Desktop.Configuration1.GetSettings",
], text=True)
assert "Catppuccin" in raw

# Application list CAS update (dock.pinned)
params = GLib.VariantBuilder.new(GLib.VariantType("(savav)"))
params.add_value(GLib.Variant.new_string("dock.pinned"))
expected = GLib.VariantBuilder.new(GLib.VariantType("av"))
params.add_value(expected.end())
value = GLib.VariantBuilder.new(GLib.VariantType("av"))
value.add_value(GLib.Variant.new_variant(GLib.Variant.new_string("firefox.desktop")))
value.add_value(GLib.Variant.new_variant(GLib.Variant.new_string("org.gnome.Nautilus.desktop")))
params.add_value(value.end())
call("UpdateApplicationList", params.end(), None)
raw = subprocess.check_output([
    "gdbus", "call", "--session", "--dest", "org.helix.Desktop",
    "--object-path", "/org/helix/Desktop",
    "--method", "org.helix.Desktop.Configuration1.GetSettings",
], text=True)
assert "firefox.desktop" in raw and "org.gnome.Nautilus.desktop" in raw
print("application_list=OK")
print("CONFIG_SMOKE_OK")
PY
