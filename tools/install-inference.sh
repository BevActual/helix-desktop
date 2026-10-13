#!/bin/sh
# Installs the model runtime (helix-inference, the speech worker helix-transcribe and the engine libraries) under /usr/local.
# Run as root:   sudo tools/install-inference.sh
# Then restart it as yourself:   systemctl --user restart helix-inference.service
# The previous binaries are kept next to the new ones as *.helix-previous.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${HELIX_BUILD_DIR:-$ROOT/build-native-clean}
[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo $0" >&2; exit 1; }
[ -x "$BUILD/helix-inference" ] && [ -x "$BUILD/helix-transcribe" ] || { echo "build first: ninja -C $BUILD helix-inference helix-transcribe" >&2; exit 1; }
for f in helix-inference helix-transcribe; do
  [ -f "/usr/local/bin/$f" ] && cp -p "/usr/local/bin/$f" "/usr/local/bin/$f.helix-previous"
done
meson install -C "$BUILD" --no-rebuild --tags inference
restorecon -v /usr/local/bin/helix-inference /usr/local/bin/helix-transcribe 2>/dev/null || true
restorecon -Rv /usr/local/lib64/helix 2>/dev/null || true
echo "Installed. Restart the service as yourself: systemctl --user restart helix-inference.service"
