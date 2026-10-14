#!/usr/bin/env bash
# Run the live session's helix-service unit with the Rust helix-desktop for a soak, and back.
#
#   tools/soak/rust-desktop.sh enable [path/to/helix-desktop]   drop-in override + restart
#   tools/soak/rust-desktop.sh disable                          remove the override + restart
#   tools/soak/rust-desktop.sh status                           who owns org.helix.Desktop, what it reports
#   tools/soak/rust-desktop.sh capture OUT.json                 read-only snapshot for `snapshot.py diff`
#
# Only the user unit's ExecStart is overridden; nothing in /usr or the install is touched, and
# `disable` restores the C++ service. Never run both services: the unit restart replaces the owner.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
UNIT=helix-service.service
DROPIN_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/$UNIT.d"
DROPIN="$DROPIN_DIR/rust-desktop.conf"

owner() {
  busctl --user status org.helix.Desktop 2>/dev/null | sed -n 's/^PID=//p' | head -1
}

implementation() {
  gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
    --method org.helix.Desktop.Configuration1.GetSnapshot 2>/dev/null |
    grep -o "'implementation': <'[a-z]*'>" | sed "s/.*<'\\(.*\\)'>/\\1/" || true
}

wait_for_owner() {
  for _ in $(seq 1 50); do
    if gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
      --method org.helix.Desktop.Configuration1.GetSnapshot >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.2
  done
  echo "org.helix.Desktop did not come back; run '$0 disable' to restore the C++ service" >&2
  return 1
}

case "${1:-}" in
  enable)
    bin="${2:-$ROOT/target/release/helix-desktop}"
    [[ -x "$bin" ]] || bin="$ROOT/target/fast/helix-desktop"
    [[ -x "$bin" ]] || { echo "build first: cargo build --release -p helix-desktop" >&2; exit 1; }
    mkdir -p "$DROPIN_DIR"
    cat >"$DROPIN" <<CONF
[Service]
ExecStart=
ExecStart="$(realpath "$bin")"
Environment=RUST_LOG=helix_desktop=info
CONF
    systemctl --user daemon-reload
    systemctl --user restart "$UNIT"
    wait_for_owner
    echo "now serving: $(implementation) (pid $(owner)), binary $(realpath "$bin")"
    echo "journal: journalctl --user -u $UNIT -f"
    ;;
  disable)
    rm -f "$DROPIN"
    rmdir "$DROPIN_DIR" 2>/dev/null || true
    systemctl --user daemon-reload
    systemctl --user restart "$UNIT"
    wait_for_owner
    echo "now serving: $(implementation) (pid $(owner))"
    ;;
  status)
    systemctl --user --no-pager status "$UNIT" | sed -n 1,12p || true
    [[ -f "$DROPIN" ]] && echo "override: $DROPIN" || echo "override: none (C++ helix-service)"
    echo "implementation: $(implementation)"
    ;;
  capture)
    [[ -n "${2:-}" ]] || { echo "usage: $0 capture OUT.json" >&2; exit 2; }
    exec python3 "$ROOT/tools/soak/snapshot.py" capture "$2"
    ;;
  *)
    sed -n 2,11p "$0"
    exit 2
    ;;
esac
