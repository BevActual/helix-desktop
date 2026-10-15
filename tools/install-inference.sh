#!/bin/sh
# Installs the model runtime (helix-inference, the speech worker helix-transcribe and the engine libraries) under /usr/local.
# Run as root:   sudo tools/install-inference.sh
# Then restart it as yourself:   systemctl --user restart helix-inference.service
# The previous binaries are kept next to the new ones as *.helix-previous.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${HELIX_BUILD_DIR:-$ROOT/build-native-clean}
INFERENCE=${HELIX_INFERENCE_BIN:-$ROOT/target/release/helix-inference}   # Rust: cargo build --release -p helix-inference
if [ "$(id -u)" -ne 0 ]; then
  if [ -n "${SUDO_ASKPASS:-}" ] && command -v sudo >/dev/null 2>&1; then
    exec sudo -A "$0" "$@"
  fi
  echo "run as root: sudo -A $0   (set SUDO_ASKPASS to ~/.local/bin/helix-askpass after tools/switch-shell.sh install)" >&2
  exit 1
fi
[ -x "$INFERENCE" ] && [ -x "$BUILD/helix-transcribe" ] || { echo "build first: cargo build --release -p helix-inference; ninja -C $BUILD helix-transcribe" >&2; exit 1; }
for f in helix-inference helix-transcribe; do
  [ -f "/usr/local/bin/$f" ] && cp -p "/usr/local/bin/$f" "/usr/local/bin/$f.helix-previous"
done
meson install -C "$BUILD" --no-rebuild --tags inference   # the worker and the engine libraries
install -D -m 0755 "$INFERENCE" /usr/local/bin/helix-inference.new && mv -f /usr/local/bin/helix-inference.new /usr/local/bin/helix-inference
restorecon -v /usr/local/bin/helix-inference /usr/local/bin/helix-transcribe 2>/dev/null || true
restorecon -Rv /usr/local/lib64/helix 2>/dev/null || true
echo "Installed. Restart the service as yourself: systemctl --user restart helix-inference.service"
