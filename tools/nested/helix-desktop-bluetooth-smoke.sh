#!/usr/bin/env bash
# Private-bus smoke for Rust helix-desktop Bluetooth1.
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

gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Bluetooth1.GetSnapshot | grep -q 'Helix Earbuds'

gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Bluetooth1.Action enable "{'value': <false>}" >/dev/null
snap="$(gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Bluetooth1.GetSnapshot)"
echo "$snap" | grep -q "'enabled': <false>"

gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Bluetooth1.Action enable "{'value': <true>}" >/dev/null
gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Bluetooth1.Action connect "{'path': <'/org/helix/mock/bt/2'>}" >/dev/null
snap="$(gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Bluetooth1.GetSnapshot)"
echo "$snap" | grep -q 'Phone'
echo "$snap" | grep -q "'connected': <true>"

gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Bluetooth1.Action pair "{'path': <'/org/helix/mock/bt/3'>}" >/dev/null
snap="$(gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Bluetooth1.GetSnapshot)"
echo "$snap" | grep -q 'Nearby Speaker'
echo "$snap" | grep -q "'paired': <true>"

gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Configuration1.GetSnapshot | grep -q "'bluetooth': <true>"

echo "BLUETOOTH_SMOKE_OK"
