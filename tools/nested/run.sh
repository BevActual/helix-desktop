#!/bin/bash
# Runs a program inside a nested niri on a private D-Bus, with its own HOME and XDG dirs, so it never touches the real session.
#   tools/nested/run.sh <name> <program> [ENV=VALUE ...]
# Leaves <scratch>/<name>/screen.png (niri screenshot incl. layer surfaces), shots/, niri.log and program.log.
# Env: HELIX_NESTED_SCRIPT=test.sh  HELIX_NESTED_SETTLE=seconds  HELIX_NIRI_BIN=path/to/niri
#      HELIX_NESTED_HEADLESS=1 (default via run.sh) uses compositor headless backend; HELIX_NESTED_WINIT=1 uses host winit.
# Never use `pkill -f` here: it matches the calling shell. Processes are stopped by PID.
set -u
NAME=${1:?name}; PROGRAM=${2:?program}; shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SCRATCH=${HELIX_NESTED_DIR:-${TMPDIR:-/tmp}/helix-nested}/$NAME
SETTLE=${HELIX_NESTED_SETTLE:-14}
if [ -z "${HELIX_NESTED_INNER:-}" ]; then
  mkdir -p "$SCRATCH/shots"
  exec env HELIX_NESTED_INNER=1 dbus-run-session --config-file="$HERE/bus.conf" -- "$0" "$NAME" "$PROGRAM" "$@"
fi
export HOME=$SCRATCH/home XDG_CONFIG_HOME=$SCRATCH/config XDG_STATE_HOME=$SCRATCH/state XDG_DATA_HOME=$SCRATCH/data XDG_CACHE_HOME=$SCRATCH/cache
export XDG_RUNTIME_DIR="$XDG_STATE_HOME/run"
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_STATE_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
# The shell's command socket and tool list go in the scratch directory, never the real runtime directory.
export HELIX_AGENT_IPC_DIR=$XDG_STATE_HOME
# Approvals on a compositor that cannot protect the dialog are refused unless the person allows them. The nested compositor is stock niri, so
# tests allow them (the protection tests unset this).
export HELIX_ALLOW_UNPROTECTED_APPROVALS=${HELIX_ALLOW_UNPROTECTED_APPROVALS-1}
# The real system bus (NetworkManager, BlueZ, logind, polkit) is out of reach of everything started here. A test that needs one
# provides its own with HELIX_NESTED_SYSTEM_BUS=unix:path=...; otherwise services see "unavailable", never the person's real devices.
export DBUS_SYSTEM_BUS_ADDRESS="${HELIX_NESTED_SYSTEM_BUS:-unix:path=/nonexistent/helix-nested-system-bus}"
unset HELIX_SHELL_ROLE HELIX_SHELL_CONFIRMER NIRI_SOCKET HELIX_STATE_HOME HELIX_CONFIG_HOME HELIX_DATA_HOME HELIX_CACHE_HOME
for kv in "$@"; do export "$kv"; done
# HELIX_NESTED_ASSISTANT=<path to helix-assistant>: pre-grant observe and act to that binary, as if the person had approved it once.
# HELIX_NESTED_ASSISTANT_TIERS (default `observe act`) and HELIX_NESTED_ASSISTANT_SCRIPT (an interpreter is known by the script it runs).
if [ -n "${HELIX_NESTED_ASSISTANT:-}" ]; then
  mkdir -p "$XDG_STATE_HOME/helix/agent"
  TIERS=$(printf '"%s",' ${HELIX_NESTED_ASSISTANT_TIERS:-observe act}); TIERS=${TIERS%,}
  printf '{"clients":{"exe:%s":[%s]},"version":1}' "$(readlink -f "$HELIX_NESTED_ASSISTANT")${HELIX_NESTED_ASSISTANT_SCRIPT:+#$HELIX_NESTED_ASSISTANT_SCRIPT}" "$TIERS" > "$XDG_STATE_HOME/helix/agent/grants.json"
  chmod 600 "$XDG_STATE_HOME/helix/agent/grants.json"
fi
cat > "$SCRATCH/niri.kdl" <<KDL
hotkey-overlay { skip-at-startup; }
screenshot-path "$SCRATCH/shots/shot-%H%M%S.png"
spawn-at-startup "sh" "-c" "exec '$PROGRAM' > '$SCRATCH/program.log' 2>&1"
KDL
# HELIX_NESTED_KDL: extra niri configuration for this run (a test of compositor-side behaviour, such as blur rules).
[ -n "${HELIX_NESTED_KDL:-}" ] && printf '%s\n' "$HELIX_NESTED_KDL" >> "$SCRATCH/niri.kdl"
# HELIX_NIRI_BIN: a compositor build to test (the fork under development); otherwise the installed one.
# HELIX_NESTED_WINIT=1: use winit on the host display instead of headless (blur/visual tests).
if [ -z "${HELIX_NESTED_WINIT:-}" ]; then
  export HELIX_NESTED_HEADLESS=1
fi
# The compositor is part of the workspace: target/{debug,fast,release}/niri (CARGO_TARGET_DIR when set), else niri on PATH.
NIRI_BIN="${HELIX_NIRI_BIN:-}"
if [ -z "$NIRI_BIN" ]; then
  for profile in debug fast release; do
    [ -x "${CARGO_TARGET_DIR:-$ROOT/target}/$profile/niri" ] && { NIRI_BIN="${CARGO_TARGET_DIR:-$ROOT/target}/$profile/niri"; break; }
  done
fi
[ -x "${NIRI_BIN:-}" ] || NIRI_BIN=niri
"$NIRI_BIN" -c "$SCRATCH/niri.kdl" > "$SCRATCH/niri.log" 2>&1 &
NIRI=$!
for _ in $(seq 1 60); do grep -q "IPC listening" "$SCRATCH/niri.log" && break; sleep 0.3; done
if ! grep -q "IPC listening" "$SCRATCH/niri.log"; then
  echo "nested niri failed to start (see $SCRATCH/niri.log)" >&2
  tail -20 "$SCRATCH/niri.log" >&2
  exit 1
fi
NIRI_SOCKET=$(grep -oE 'IPC listening on: [^ ]+' "$SCRATCH/niri.log" | tail -1 | sed 's/IPC listening on: //')
if [ -z "$NIRI_SOCKET" ]; then
  NIRI_SOCKET=$(grep -oE "/run/user/[0-9]+/niri\.[^ ]*\.sock" "$SCRATCH/niri.log" | tail -1)
fi
export NIRI_SOCKET
DISPLAY_NAME=$(grep -oE "wayland-[0-9]+" "$SCRATCH/niri.log" | tail -1)
sleep "$SETTLE"
if [ -n "${HELIX_NESTED_SCRIPT:-}" ]; then WAYLAND_DISPLAY=$DISPLAY_NAME bash ${HELIX_NESTED_BASH_FLAGS:-} "$HELIX_NESTED_SCRIPT" "$SCRATCH"; fi
niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5
ls -t "$SCRATCH"/shots/*.png 2>/dev/null | head -1 | xargs -I{} cp {} "$SCRATCH/screen.png"
kill "$NIRI" 2>/dev/null; sleep 1; kill -9 "$NIRI" 2>/dev/null
# Anything the program started that must not outlive the run, stopped by pid (never by name).
for f in "$XDG_STATE_HOME/pipewire.pid" "$XDG_STATE_HOME/helix-service.pid"; do
  [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null
done
echo "screenshot: $SCRATCH/screen.png"
