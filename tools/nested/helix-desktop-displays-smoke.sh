#!/usr/bin/env bash
# Private-bus smoke for Rust helix-desktop Displays1.
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

snap="$(gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Displays1.GetSnapshot)"
echo "$snap" | grep -q "'available': <false>"

gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Configuration1.RegisterDesktop >/dev/null

snap="$(gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Displays1.GetSnapshot)"
echo "$snap" | grep -q "'available': <true>"
echo "$snap" | grep -q 'eDP-1'
echo "$snap" | grep -q 'inventory'

echo "DISPLAYS_SMOKE_OK"
