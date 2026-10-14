#!/usr/bin/env bash
# Audio1 input meter parity: desktop_audio_meter_integration.py against Rust helix-desktop.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${HELIX_SERVICE_BIN:-$ROOT/target/fast/helix-desktop}"
CONF="${ROOT}/tests/desktop_audio/pipewire.conf"

cargo build --profile fast -p helix-desktop --quiet
python3 -c 'from gi.repository import Gio, GLib' 2>/dev/null || {
  echo "python3-gobject (PyGObject) required for audio meter integration" >&2
  exit 1
}
command -v dbus-run-session >/dev/null || {
  echo "dbus-run-session required" >&2
  exit 1
}
for tool in pipewire pw-cli pw-link pw-dump; do
  command -v "$tool" >/dev/null || {
    echo "$tool required for audio meter integration" >&2
    exit 1
  }
done

dbus-run-session -- python3 "$ROOT/tests/desktop_audio_meter_integration.py" "$BIN" "$CONF"
