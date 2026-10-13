#!/bin/sh
# Installs the Helix greeter for greetd. Run as root:   sudo tools/install-greeter.sh
# Undo (restores the previous greetd configuration):    sudo tools/install-greeter.sh --revert
# Takes effect at the next login screen (it does not restart greetd, so the current session is untouched).
# The previous /etc/greetd/config.toml is kept as config.toml.helix-previous.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CONFIG=/etc/greetd/config.toml
KEEP=/etc/greetd/config.toml.helix-previous
[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo $0" >&2; exit 1; }

if [ "${1:-}" = "--revert" ]; then
  [ -f "$KEEP" ] || { echo "no previous configuration at $KEEP" >&2; exit 1; }
  cp -p "$KEEP" "$CONFIG"
  echo "Restored $CONFIG from $KEEP. Takes effect at the next login screen."
  exit 0
fi

BIN=$ROOT/target/release/helix-greeter
[ -x "$BIN" ] || { echo "build it first: cargo build --release -p helix-greeter" >&2; exit 1; }
[ -x /usr/bin/niri ] || { echo "the system niri (/usr/bin/niri) is required for the greeter's compositor" >&2; exit 1; }
[ -x /usr/bin/agreety ] || echo "warning: /usr/bin/agreety is missing, so there is no text-login fallback" >&2

install -D -m 0755 "$BIN" /usr/local/bin/helix-greeter.new && mv -f /usr/local/bin/helix-greeter.new /usr/local/bin/helix-greeter
# In /usr/local/bin on purpose: SELinux labels it bin_t, which greetd (xdm_t) may execute; /usr/local/libexec is usr_t.
install -D -m 0755 "$ROOT/greeter/session/helix-greeter-session" /usr/local/bin/helix-greeter-session
rm -f /usr/local/libexec/helix-greeter-session
command -v restorecon >/dev/null 2>&1 && restorecon -R /usr/local/bin/helix-greeter /usr/local/bin/helix-greeter-session /usr/local/share/helix-greeter || true
install -D -m 0644 "$ROOT/greeter/session/greeter-niri.kdl" /usr/local/share/helix-greeter/niri.kdl
/usr/bin/niri validate -c /usr/local/share/helix-greeter/niri.kdl >/dev/null

[ -f "$CONFIG" ] && [ ! -f "$KEEP" ] && cp -p "$CONFIG" "$KEEP"
VT=$(sed -n 's/^vt *= *//p' "$CONFIG" 2>/dev/null | head -1)
cat > "$CONFIG.new" <<EOF
[terminal]
vt = ${VT:-1}

[default_session]
command = "/usr/local/bin/helix-greeter-session"
user = "greetd"
EOF
mv -f "$CONFIG.new" "$CONFIG"
echo "Installed. greetd will start the Helix greeter at the next login screen."
echo "If it cannot start, greetd falls back to a text login. To go back:  sudo $0 --revert"
