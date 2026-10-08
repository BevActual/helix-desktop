#!/bin/bash
# Started by tools/nested/run.sh inside the private bus. Starts helix-service, then the Tauri shell.
# The service pid is recorded so the nested script can stop it by pid. This never touches the real session.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# Unless HELIX_SHELL_BIN says otherwise, run whichever build (fast or release) was made last, so a test never silently runs a stale binary.
newest_shell_bin() { ls -t "$ROOT/rust/target/fast/helix-shell-tauri" "$ROOT/rust/target/release/helix-shell-tauri" 2>/dev/null | head -1; }
export PIPEWIRE_REMOTE="${PIPEWIRE_REMOTE:-helix-nested-no-audio}"
# Agent1 fails closed without a risk policy. Prefer an explicit path; otherwise the installed datadir.
if [ -z "${HELIX_RISK_POLICY:-}" ] && [ -f "$ROOT/protocols/ipc/msg-risk.toml" ]; then
  export HELIX_RISK_POLICY="$ROOT/protocols/ipc/msg-risk.toml"
fi
# Nested harness owns Agent1 confirmer. The live trial binary must leave confirmer to the C++ shell.
export HELIX_SHELL_CONFIRMER=1
# HELIX_NESTED_PRIVATE_AUDIO=1: a private PipeWire with virtual sinks and a microphone, so audio writes can be tested for real
# without touching the person's hardware. It is stopped by pid at the end of the run (tools/nested/run.sh).
if [ -n "${HELIX_NESTED_PRIVATE_AUDIO:-}" ]; then
  eval "$("$(dirname "$0")/private-audio.sh" "${XDG_STATE_HOME}/pipewire")"
  echo "$HELIX_PRIVATE_PIPEWIRE_PID" > "${XDG_STATE_HOME}/pipewire.pid"
fi
# A stand-in backlight, so a nested session never reads or writes the real one.
mkdir -p "${XDG_STATE_HOME}/backlight"; printf "50\n" > "${XDG_STATE_HOME}/backlight/brightness"; printf "100\n" > "${XDG_STATE_HOME}/backlight/max_brightness"
export HELIX_BACKLIGHT_DIR="${XDG_STATE_HOME}/backlight"
export HELIX_SURFACE_CONTROL="${XDG_STATE_HOME}/surface-control"
export HELIX_UI_LAYOUT_LOG="${XDG_STATE_HOME}/layout.json"   # where the page reports its named controls (tests click by name)
SERVICE="$ROOT/build-debug/helix-service"
SHELL_BIN="${HELIX_SHELL_BIN:-$(newest_shell_bin)}"
"$SERVICE" > "${XDG_STATE_HOME}/helix-service.log" 2>&1 &
echo $! > "${XDG_STATE_HOME}/helix-service.pid"
# Record this pid before exec replaces the script. Do not call the bus here: that activates the
# installed service in the real HOME. The shell retries until this process owns the name.
echo $$ > "${XDG_STATE_HOME}/shell.pid"
exec "$SHELL_BIN"
