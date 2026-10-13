#!/bin/bash
# Starts a private PipeWire (see private-pipewire.conf) and prints, on stdout, the PID to stop and nothing else on success.
#   eval "$(tools/nested/private-audio.sh <runtime-dir>)"   -> exports PIPEWIRE_RUNTIME_DIR, PIPEWIRE_REMOTE and HELIX_PRIVATE_PIPEWIRE_PID
# Real audio is not touched: the socket lives in <runtime-dir>, and there is no session manager to open hardware.
set -eu
DIR=${1:?runtime dir}; HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$DIR"; chmod 700 "$DIR"
PIPEWIRE_RUNTIME_DIR="$DIR" PIPEWIRE_CONFIG_FILE="$HERE/private-pipewire.conf" pipewire -c "$HERE/private-pipewire.conf" > "$DIR/pipewire.log" 2>&1 &
PID=$!
for _ in $(seq 1 40); do [ -S "$DIR/pipewire-helix" ] && break; sleep 0.1; done
[ -S "$DIR/pipewire-helix" ] || { echo "echo 'private pipewire did not start' >&2; false"; exit 1; }
echo "export PIPEWIRE_RUNTIME_DIR='$DIR' HELIX_PRIVATE_PIPEWIRE_PID=$PID PIPEWIRE_REMOTE=pipewire-helix"
