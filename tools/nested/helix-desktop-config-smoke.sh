#!/usr/bin/env bash
# Private-bus smoke for Rust helix-desktop Configuration1 catalog + runtime state.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${HELIX_SERVICE_BIN:-$ROOT/target/fast/helix-desktop}"
# The service's msg socket is $XDG_RUNTIME_DIR/helix-$WAYLAND_DISPLAY.sock. Use a name of our own so that on a machine with a
# live session this never talks to (or takes) the real shell's socket.
export WAYLAND_DISPLAY="wayland-smoke-$$"

if [[ -z "${HELIX_DESKTOP_CONFIG_SMOKE_INNER:-}" ]]; then
  cargo build --profile fast -p helix-desktop --quiet
  command -v dbus-run-session >/dev/null || {
    echo "dbus-run-session required" >&2
    exit 1
  }
  export HELIX_DESKTOP_CONFIG_SMOKE_INNER=1
  exec dbus-run-session -- env HELIX_SERVICE_BIN="$BIN" "$0"
fi

export XDG_CONFIG_HOME XDG_STATE_HOME HELIX_ASSETS_DIR
XDG_CONFIG_HOME="$(mktemp -d)"
XDG_STATE_HOME="$(mktemp -d)"
mkdir -p "$XDG_CONFIG_HOME/helix"
cat >"$XDG_CONFIG_HOME/helix/config.toml" <<'EOF'
[bar.main]
enabled = true
thickness = 34

[idle.behavior.lock]
timeout = 420
enabled = true
action = "lock"

# Hook-free built-in templates: exercised in-process by templates-apply below.
[theme.templates]
builtin_ids = ["helix", "qt"]
EOF
HELIX_ASSETS_DIR="$ROOT/assets"
for _candidate in \
  "$ROOT/_build/helix-template-apply" \
  "$ROOT/build-debug/helix-template-apply"; do
  if [[ -z "${HELIX_TEMPLATE_APPLY_BIN:-}" && -x "$_candidate" ]]; then
    HELIX_TEMPLATE_APPLY_BIN="$_candidate"
  fi
done
export HELIX_TEMPLATE_APPLY_BIN
for _candidate in \
  "$ROOT/_build/helix-theme-palette" \
  "$ROOT/build-debug/helix-theme-palette"; do
  if [[ -z "${HELIX_THEME_PALETTE_BIN:-}" && -x "$_candidate" ]]; then
    HELIX_THEME_PALETTE_BIN="$_candidate"
  fi
done
export HELIX_THEME_PALETTE_BIN
for _candidate in \
  "$ROOT/_build/helix-config-export" \
  "$ROOT/build-debug/helix-config-export"; do
  if [[ -z "${HELIX_CONFIG_EXPORT_BIN:-}" && -x "$_candidate" ]]; then
    HELIX_CONFIG_EXPORT_BIN="$_candidate"
  fi
done
export HELIX_CONFIG_EXPORT_BIN
"$BIN" &
PID=$!
trap 'kill "$PID" 2>/dev/null || true' EXIT
for _ in $(seq 1 40); do
  gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
    --method org.freedesktop.DBus.NameHasOwner org.helix.Desktop 2>/dev/null | grep -q true && break
  sleep 0.05
done

python3 - <<'PY'
import os
import re
import subprocess
from pathlib import Path

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

def appearance_call(method, params=None, reply=GLib.VariantType("()")):
    return bus.call_sync(
        "org.helix.Desktop",
        "/org/helix/Desktop",
        "org.helix.Desktop.Appearance1",
        method,
        params,
        reply,
        Gio.DBusCallFlags.NONE,
        -1,
        None,
    )

# Catalog before a shell registers: portable rows only (C++ contract).
def settings_ids():
    raw = subprocess.check_output([
        "gdbus", "call", "--session", "--dest", "org.helix.Desktop",
        "--object-path", "/org/helix/Desktop",
        "--method", "org.helix.Desktop.Configuration1.GetSettings",
    ], text=True)
    return re.findall(r"'id': <'([^']+)'>", raw), raw

portable_ids, portable_raw = settings_ids()
print("portable_setting_count=", len(portable_ids))
assert "theme.mode" in portable_ids and "dock.enabled" not in portable_ids, portable_ids
for page in ("desktop-dock", "keyboard", "notifications", "power", "system", "accessibility"):
    assert f"'page': <'{page}'>" not in portable_raw, page


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

# Full catalog once a shell is registered and the compositor offers gamma control.
ids, _raw = settings_ids()
print("setting_count=", len(ids))
assert len(ids) >= 300, len(ids)
for want in [
    "nightlight.force", "nightlight.temperature_day", "theme.builtin", "wallpaper.enabled",
    "shell.offline_mode", "notification.enable_daemon", "notification.position", "notification.layer",
    "control_center.width", "osd.kinds.volume", "osd.position", "osd.kinds.wifi",
    "dock.enabled", "dock.position", "dock.launcher_position",
    "hot_corners.delay_ms", "wallpaper.fill_mode", "wallpaper.automation.enabled",
    "shell.clipboard_auto_paste", "lockscreen.blurred_desktop", "lockscreen.lock_before_suspend",
    "theme.shell_mode", "theme.niri_layout.overview", "location.latitude",
    "shell.external_ip_enabled", "shell.launcher.provider_prefix",
    "shell.window_switcher.mru", "shell.session.grid_columns",
]:
    assert want in ids, want
print("catalog=OK")
snap = call("GetSnapshot")
fields = dict(snap.unpack()[0])
assert fields["helix-desktop"] is True
caps_out = dict(fields["desktop-capabilities"])
assert caps_out["gamma-control"] is True
print("capabilities=OK")

raw = subprocess.check_output([
    "gdbus", "call", "--session", "--dest", "org.helix.Desktop",
    "--object-path", "/org/helix/Desktop",
    "--method", "org.helix.Desktop.Configuration1.GetSettings",
], text=True)
desktop_ids = re.findall(r"'id': <'([^']+)'>", raw)
assert "bar:main:enabled" in desktop_ids, desktop_ids
assert "idle:lock:timeout" in desktop_ids, desktop_ids
assert len(desktop_ids) >= len(portable_ids) + 20, (len(desktop_ids), len(portable_ids))
print("dynamic_desktop_catalog=OK")

params = GLib.VariantBuilder.new(GLib.VariantType("(sv)"))
params.add_value(GLib.Variant.new_string("idle:lock:timeout"))
params.add_value(GLib.Variant.new_variant(GLib.Variant.new_double(500)))
call("Update", params.end(), None)
raw = subprocess.check_output([
    "gdbus", "call", "--session", "--dest", "org.helix.Desktop",
    "--object-path", "/org/helix/Desktop",
    "--method", "org.helix.Desktop.Configuration1.GetSettings",
], text=True)
assert "'id': <'idle:lock:timeout'>" in raw
assert "500" in raw
print("idle_update=OK")

params = GLib.VariantBuilder.new(GLib.VariantType("(s)"))
params.add_value(GLib.Variant.new_string("idle.behavior"))
call("Reset", params.end(), None)
raw = subprocess.check_output([
    "gdbus", "call", "--session", "--dest", "org.helix.Desktop",
    "--object-path", "/org/helix/Desktop",
    "--method", "org.helix.Desktop.Configuration1.GetSettings",
], text=True)
assert "420" in raw
print("idle_reset=OK")

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

params = GLib.VariantBuilder.new(GLib.VariantType("(savav)"))
params.add_value(GLib.Variant.new_string("dock.pinned"))
expected = GLib.VariantBuilder.new(GLib.VariantType("av"))
expected.add_value(GLib.Variant.new_variant(GLib.Variant.new_string("firefox.desktop")))
expected.add_value(GLib.Variant.new_variant(GLib.Variant.new_string("org.gnome.Nautilus.desktop")))
params.add_value(expected.end())
value = GLib.VariantBuilder.new(GLib.VariantType("av"))
value.add_value(GLib.Variant.new_variant(GLib.Variant.new_string("org.gnome.Nautilus.desktop")))
params.add_value(value.end())
call("UpdateOrderedList", params.end(), None)
raw = subprocess.check_output([
    "gdbus", "call", "--session", "--dest", "org.helix.Desktop",
    "--object-path", "/org/helix/Desktop",
    "--method", "org.helix.Desktop.Configuration1.GetSettings",
], text=True)
assert "firefox.desktop" not in raw
print("ordered_list=OK")

params = GLib.VariantBuilder.new(GLib.VariantType("(savav)"))
params.add_value(GLib.Variant.new_string("control_center.shortcuts"))
expected = GLib.VariantBuilder.new(GLib.VariantType("av"))
params.add_value(expected.end())
value = GLib.VariantBuilder.new(GLib.VariantType("av"))
tile = GLib.VariantBuilder.new(GLib.VariantType("a{sv}"))
tile.add_value(
    GLib.Variant.new_dict_entry(
        GLib.Variant.new_string("type"),
        GLib.Variant.new_variant(GLib.Variant.new_string("wifi")),
    )
)
tile.add_value(
    GLib.Variant.new_dict_entry(
        GLib.Variant.new_string("size"),
        GLib.Variant.new_variant(GLib.Variant.new_string("small")),
    )
)
value.add_value(GLib.Variant.new_variant(tile.end()))
params.add_value(value.end())
call("UpdateOrderedList", params.end(), None)
raw = subprocess.check_output([
    "gdbus", "call", "--session", "--dest", "org.helix.Desktop",
    "--object-path", "/org/helix/Desktop",
    "--method", "org.helix.Desktop.Configuration1.GetSettings",
], text=True)
assert "'wifi'" in raw and "'small'" in raw
print("shortcut_list=OK")

params = GLib.VariantBuilder.new(GLib.VariantType("(savav)"))
params.add_value(GLib.Variant.new_string("wallpaper.transition"))
expected = GLib.VariantBuilder.new(GLib.VariantType("av"))
for style in ["fade", "wipe", "disc", "stripes", "zoom", "honeycomb"]:
    expected.add_value(GLib.Variant.new_variant(GLib.Variant.new_string(style)))
params.add_value(expected.end())
value = GLib.VariantBuilder.new(GLib.VariantType("av"))
value.add_value(GLib.Variant.new_variant(GLib.Variant.new_string("fade")))
value.add_value(GLib.Variant.new_variant(GLib.Variant.new_string("wipe")))
params.add_value(value.end())
call("UpdateOrderedList", params.end(), None)
raw = subprocess.check_output([
    "gdbus", "call", "--session", "--dest", "org.helix.Desktop",
    "--object-path", "/org/helix/Desktop",
    "--method", "org.helix.Desktop.Configuration1.GetSettings",
], text=True)
assert "'fade'" in raw and "'wipe'" in raw
print("transition_list=OK")

baseline_path = Path(os.environ["XDG_STATE_HOME"]) / "helix" / "settings.toml"
baseline = baseline_path.read_text()
call(
    "CommitOverrides",
    GLib.Variant("(ss)", (baseline, '[dock]\npinned = ["firefox.desktop"]\n')),
    None,
)
raw = subprocess.check_output([
    "gdbus", "call", "--session", "--dest", "org.helix.Desktop",
    "--object-path", "/org/helix/Desktop",
    "--method", "org.helix.Desktop.Configuration1.GetSettings",
], text=True)
assert "firefox.desktop" in raw
print("commit_overrides=OK")

# templates-apply msg IPC (requires RegisterDesktop above)
import socket as pysocket
runtime = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
display = os.environ.get("WAYLAND_DISPLAY", "wayland-0")
sock_path = f"{runtime}/helix-{display}.sock"
client = pysocket.socket(pysocket.AF_UNIX, pysocket.SOCK_STREAM)
client.connect(sock_path)
client.sendall(b"templates-apply")
client.shutdown(pysocket.SHUT_WR)
resp = client.recv(4096).decode()
assert resp.strip() == "ok", resp
cfg_home = os.environ["XDG_CONFIG_HOME"]
for rel in ("helix/themes/helix.toml", "qt5ct/colors/helix.conf", "qt6ct/colors/helix.conf"):
    path = os.path.join(cfg_home, rel)
    assert os.path.isfile(path) and os.path.getsize(path) > 0, path
print("templates_apply_ipc=OK")

# SetAccent runs in-process for builtin/custom palettes in a concrete mode (no helper needed).
import json
appearance_call("SetAccent", GLib.Variant("(s)", ("#336699",)))
palette_file = os.path.join(os.environ["XDG_CONFIG_HOME"], "helix", "palettes", "custom.json")
saved = json.load(open(palette_file))
assert saved["dark"]["mPrimary"] == "#336699", saved["dark"]["mPrimary"]
overrides = open(os.path.join(os.environ["XDG_STATE_HOME"], "helix", "settings.toml")).read()
assert 'source = "custom"' in overrides and 'custom_palette = "custom"' in overrides, overrides
print("theme_palette=OK")

print("CONFIG_SMOKE_OK")
PY
