#!/usr/bin/env bash
# Installs the system-level pieces of Helix: the desktop service, assistant, inference service and upgrade helper, with their
# systemd user units, D-Bus activation files, polkit policy, icon and the agent policy tables. This is what `meson install` did.
# The session shell, greeter and apps are installed for a user by tools/switch-shell.sh and tools/install-greeter.sh.
#
#   tools/install.sh [--prefix DIR] [--destdir DIR] [--profile release|fast] [--no-build]
#   tools/install.sh --uninstall [--prefix DIR] [--destdir DIR]
#
# Defaults: prefix /usr/local, profile release. DESTDIR stages the tree instead of touching the system (packagers, tests).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=${PREFIX:-/usr/local}
DESTDIR=${DESTDIR:-}
PROFILE=release
BUILD=1
UNINSTALL=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix)   PREFIX=${2:?--prefix needs a directory}; shift 2 ;;
    --destdir)  DESTDIR=${2:?--destdir needs a directory}; shift 2 ;;
    --profile)  PROFILE=${2:?--profile needs release or fast}; shift 2 ;;
    --no-build) BUILD=0; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help)  sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "install.sh: unknown argument $1" >&2; exit 2 ;;
  esac
done
case "$PROFILE" in release|fast) ;; *) echo "install.sh: profile must be release or fast" >&2; exit 2 ;; esac

BINDIR=$PREFIX/bin
LIBEXECDIR=$PREFIX/libexec
DATADIR=$PREFIX/share
UNITDIR=$PREFIX/lib/systemd/user
OUT=$ROOT/target/$PROFILE
D=$DESTDIR

if [ "$UNINSTALL" = 1 ]; then
  rm -f "$D$BINDIR"/helix-desktop "$D$BINDIR"/helix-assistant "$D$BINDIR"/helix-inference \
        "$D$BINDIR"/helix-transcribe "$D$BINDIR"/helix-image "$D$LIBEXECDIR/helix-system-upgrade" \
        "$D$UNITDIR"/helix-service.service "$D$UNITDIR"/helix-inference.service "$D$UNITDIR"/helix-assistant-warm.service \
        "$D$DATADIR"/dbus-1/services/org.helix.Desktop.service "$D$DATADIR"/dbus-1/services/org.helix.Inference.service \
        "$D$DATADIR"/polkit-1/actions/org.helix.Shell.upgrade.policy "$D$DATADIR"/icons/hicolor/scalable/apps/helix.svg \
        "$D$DATADIR"/helix/msg-risk.toml "$D$DATADIR"/helix/tool-hints.toml
  rm -rf "$D$PREFIX/lib64/helix"
  rmdir "$D$DATADIR/helix" 2>/dev/null || true
  echo "removed from ${D}${PREFIX}"
  exit 0
fi

if [ "$BUILD" = 1 ]; then
  command -v cargo >/dev/null || { echo "install.sh: cargo not found" >&2; exit 1; }
  (cd "$ROOT" && cargo build --profile "$PROFILE" -p helix-desktop -p helix-assistant -p helix-inference -p helix-system-upgrade)
fi
for bin in helix-desktop helix-assistant helix-inference helix-system-upgrade; do
  [ -x "$OUT/$bin" ] || { echo "install.sh: $OUT/$bin is missing (build it, or drop --no-build)" >&2; exit 1; }
done

install -Dm755 "$OUT/helix-desktop"   "$D$BINDIR/helix-desktop"
install -Dm755 "$OUT/helix-assistant" "$D$BINDIR/helix-assistant"
install -Dm755 "$OUT/helix-inference" "$D$BINDIR/helix-inference"
install -Dm755 "$OUT/helix-system-upgrade" "$D$LIBEXECDIR/helix-system-upgrade"
# Workers that need an engine built by hand: installed when present.
for bin in helix-transcribe helix-image; do
  if [ -x "$OUT/$bin" ]; then install -Dm755 "$OUT/$bin" "$D$BINDIR/$bin"; else echo "note: $bin not built; skipping" >&2; fi
done

# @NAME@ placeholders in the unit, activation and policy templates.
subst() { # template destination key=value...
  local src=$1 dst=$2; shift 2
  mkdir -p "$(dirname "$dst")"
  local args=()
  for kv in "$@"; do args+=(-e "s|@${kv%%=*}@|${kv#*=}|g"); done
  sed "${args[@]}" "$src" > "$dst"
  chmod 0644 "$dst"
}
for unit in helix-service helix-inference helix-assistant-warm; do
  subst "$ROOT/assets/services/$unit.service.in" "$D$UNITDIR/$unit.service" "HELIX_BINDIR=$BINDIR"
done
for name in org.helix.Desktop org.helix.Inference; do
  subst "$ROOT/assets/services/$name.service.in" "$D$DATADIR/dbus-1/services/$name.service" "HELIX_BINDIR=$BINDIR"
done
subst "$ROOT/assets/polkit/org.helix.Shell.upgrade.policy.in" "$D$DATADIR/polkit-1/actions/org.helix.Shell.upgrade.policy" \
  "HELPER_PATH=$LIBEXECDIR/helix-system-upgrade"

install -Dm644 "$ROOT/assets/helix.svg" "$D$DATADIR/icons/hicolor/scalable/apps/helix.svg"
install -Dm644 "$ROOT/protocols/ipc/msg-risk.toml"  "$D$DATADIR/helix/msg-risk.toml"
install -Dm644 "$ROOT/protocols/ipc/tool-hints.toml" "$D$DATADIR/helix/tool-hints.toml"

# The model engine's shared libraries, when it was built into third_party/llama-install.
ENGINE=$ROOT/third_party/llama-install/lib64
if [ -e "$ENGINE/libllama.so" ]; then
  PREFIX=$PREFIX DESTDIR=$DESTDIR python3 "$ROOT/tools/scripts/install_engine.py" "$ENGINE" lib64/helix
fi

echo "installed to ${D}${PREFIX}"
