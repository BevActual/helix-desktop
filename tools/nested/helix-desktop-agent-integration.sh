#!/usr/bin/env bash
# Agent Surface parity: desktop_agent_integration.py against Rust helix-desktop.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${HELIX_SERVICE_BIN:-$ROOT/target/debug/helix-desktop}"
POLICY="${HELIX_RISK_POLICY:-$ROOT/protocols/ipc/msg-risk.toml}"

cargo build -p helix-desktop --quiet
python3 -c 'from gi.repository import Gio, GLib' 2>/dev/null || {
  echo "python3-gobject (PyGObject) required for agent integration tests" >&2
  exit 1
}
command -v dbus-run-session >/dev/null || {
  echo "dbus-run-session required" >&2
  exit 1
}

dbus-run-session -- python3 "$ROOT/tests/desktop_agent_integration.py" "$BIN" "$POLICY"
dbus-run-session -- python3 "$ROOT/tests/desktop_agent_manager_integration.py" "$BIN" "$POLICY"
