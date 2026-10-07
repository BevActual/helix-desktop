#!/bin/bash
# Runs a program inside a nested niri on a private D-Bus, with its own HOME and XDG dirs, so it never touches the real session.
#   tools/nested/run.sh <name> <program> [ENV=VALUE ...]
# Leaves <scratch>/<name>/screen.png (niri screenshot incl. layer surfaces), shots/, niri.log and program.log.
# Never use `pkill -f` here: it matches the calling shell. Processes are stopped by PID.
set -u
NAME=${1:?name}; PROGRAM=${2:?program}; shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
SCRATCH=${HELIX_NESTED_DIR:-${TMPDIR:-/tmp}/helix-nested}/$NAME
SETTLE=${HELIX_NESTED_SETTLE:-14}
if [ -z "${HELIX_NESTED_INNER:-}" ]; then
  mkdir -p "$SCRATCH/shots"
  exec env HELIX_NESTED_INNER=1 dbus-run-session --config-file="$HERE/bus.conf" -- "$0" "$NAME" "$PROGRAM" "$@"
fi
export HOME=$SCRATCH/home XDG_CONFIG_HOME=$SCRATCH/config XDG_STATE_HOME=$SCRATCH/state XDG_DATA_HOME=$SCRATCH/data XDG_CACHE_HOME=$SCRATCH/cache
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_STATE_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME"
unset NIRI_SOCKET NOCTALIA_STATE_HOME NOCTALIA_CONFIG_HOME NOCTALIA_DATA_HOME NOCTALIA_CACHE_HOME
for kv in "$@"; do export "$kv"; done
cat > "$SCRATCH/niri.kdl" <<KDL
hotkey-overlay { skip-at-startup; }
screenshot-path "$SCRATCH/shots/shot-%H%M%S.png"
spawn-at-startup "sh" "-c" "exec '$PROGRAM' > '$SCRATCH/program.log' 2>&1"
KDL
niri -c "$SCRATCH/niri.kdl" > "$SCRATCH/niri.log" 2>&1 &
NIRI=$!
for _ in $(seq 1 60); do grep -q "IPC listening" "$SCRATCH/niri.log" && break; sleep 0.3; done
NIRI_SOCKET=$(grep -oE "/run/user/[0-9]+/niri\.[^ ]*\.sock" "$SCRATCH/niri.log" | tail -1)
export NIRI_SOCKET
DISPLAY_NAME=$(grep -oE "wayland-[0-9]+" "$SCRATCH/niri.log" | tail -1)
sleep "$SETTLE"
if [ -n "${HELIX_NESTED_SCRIPT:-}" ]; then WAYLAND_DISPLAY=$DISPLAY_NAME bash "$HELIX_NESTED_SCRIPT" "$SCRATCH"; fi
niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5
ls -t "$SCRATCH"/shots/*.png 2>/dev/null | head -1 | xargs -I{} cp {} "$SCRATCH/screen.png"
kill "$NIRI" 2>/dev/null; sleep 1; kill -9 "$NIRI" 2>/dev/null
echo "screenshot: $SCRATCH/screen.png"
