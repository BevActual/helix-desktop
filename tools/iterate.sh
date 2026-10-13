#!/bin/bash
# Fast edit loop for the Tauri session shell. Rebuilds with the quick profile and points the unit
# at that binary. Does **not** restart the live shell (restarting crashes Cursor on this session).
#   tools/iterate.sh           build --profile fast + drop-in ExecStart
#   tools/iterate.sh release   clear drop-in; build --release
#   tools/iterate.sh --restart same, then bounce the shell (prefer a plain terminal)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DROP=$HOME/.config/systemd/user/helix-shell-tauri.service.d
RESTART=0
ARGS=()
for arg in "$@"; do
  case "$arg" in
    --restart) RESTART=1 ;;
    *) ARGS+=("$arg") ;;
  esac
done
set -- "${ARGS[@]:-fast}"

case "${1:-fast}" in
  release)
    rm -f "$DROP/iterate.conf"; rmdir "$DROP" 2>/dev/null || true
    systemctl --user daemon-reload
    (cd "$ROOT" && cargo build --release -p helix-shell-tauri 2>&1 | tail -3)
    ;;
  *)
    (cd "$ROOT" && cargo build --profile fast -p helix-shell-tauri 2>&1 | grep -E '^(error|warning: unused)|Finished' -A6 || true)
    mkdir -p "$DROP"
    BIN=$(printf '%s' "$ROOT/target/fast/helix-shell-tauri" | sed 's/ /\\x20/g')
    printf '[Service]\nExecStart=\nExecStart=%s\n' "$BIN" > "$DROP/iterate.conf"
    systemctl --user daemon-reload
    ;;
esac

if [ "$RESTART" = 1 ]; then
  echo "Restarting helix-shell-tauri…"
  systemctl --user restart helix-shell-tauri.service
  sleep 2
  systemctl --user is-active helix-shell-tauri.service
else
  echo "Unit points at the new binary. Shell not restarted (avoids crashing Cursor)."
  echo "When ready: tools/switch-shell.sh restart   # or: $0 $* --restart"
fi
