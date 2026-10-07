#!/bin/bash
# Started by tools/nested/run.sh inside the private bus. Starts helix-service, then the Tauri shell.
# The service pid is recorded so the nested script can stop it by pid. This never touches the real session.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export PIPEWIRE_REMOTE="${PIPEWIRE_REMOTE:-helix-nested-no-audio}"
export HELIX_SURFACE_CONTROL="${XDG_STATE_HOME}/surface-control"
SERVICE="$ROOT/build-debug/helix-service"
SHELL_BIN="$ROOT/rust/target/release/helix-shell-tauri"
"$SERVICE" > "${XDG_STATE_HOME}/helix-service.log" 2>&1 &
echo $! > "${XDG_STATE_HOME}/helix-service.pid"
# Record this pid before exec replaces the script. Do not call the bus here: that activates the
# installed service in the real HOME. The shell retries until this process owns the name.
echo $$ > "${XDG_STATE_HOME}/shell.pid"
exec "$SHELL_BIN"
