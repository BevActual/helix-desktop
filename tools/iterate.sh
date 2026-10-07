#!/bin/bash
# Fast edit-and-look loop on the live desktop. `tools/iterate.sh` rebuilds the Tauri shell with the quick profile and restarts the
# session shell onto it; `tools/iterate.sh release` goes back to the release binary. Only the shell unit changes (a drop-in under
# ~/.config/systemd/user), never /usr/local.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DROP=$HOME/.config/systemd/user/helix-shell-tauri.service.d
case "${1:-fast}" in
  release)
    rm -f "$DROP/iterate.conf"; rmdir "$DROP" 2>/dev/null || true
    systemctl --user daemon-reload
    (cd "$ROOT/rust" && cargo build --release 2>&1 | tail -1)
    ;;
  *)
    (cd "$ROOT/rust" && cargo build --profile fast 2>&1 | grep -E '^(error|warning: unused)|Finished' -A6)
    mkdir -p "$DROP"
    BIN=$(printf '%s' "$ROOT/rust/target/fast/helix-shell-tauri" | sed 's/ /\\x20/g')
    printf '[Service]\nExecStart=\nExecStart=%s\n' "$BIN" > "$DROP/iterate.conf"
    systemctl --user daemon-reload
    ;;
esac
systemctl --user restart helix-shell-tauri.service
sleep 3
systemctl --user is-active helix-shell-tauri.service
