#!/usr/bin/env bash
# Installs Helix under a prefix, the way a package does.
#
# Always: the desktop service, assistant, inference service and upgrade helper, with their systemd user units, D-Bus activation
# files, polkit policy, icon, theme templates and the agent policy tables.
# Unless --services-only: the session shell and strut, wallpaper and idle helpers, the greeter and its session script, Settings,
# Mobius, Software and System Monitor, with their desktop entries, icons and systemd user units, and the helix-panel and
# helix-askpass scripts. Pieces that were not built are skipped with a note; --require-session makes that an error.
#
# Not touched: the compositor (tools/install-trusted-niri.sh), greetd's configuration (tools/install-greeter.sh) and the user's niri
# configuration. For a single user from a source checkout, tools/switch-shell.sh install links the build tree instead.
#
#   tools/install.sh [--prefix DIR] [--destdir DIR] [--profile release|fast] [--no-build] [--services-only] [--require-session]
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
SESSION=1
REQUIRE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix)   PREFIX=${2:?--prefix needs a directory}; shift 2 ;;
    --destdir)  DESTDIR=${2:?--destdir needs a directory}; shift 2 ;;
    --profile)  PROFILE=${2:?--profile needs release or fast}; shift 2 ;;
    --no-build) BUILD=0; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    --services-only) SESSION=0; shift ;;
    --require-session) REQUIRE=1; shift ;;
    -h|--help)  sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "install.sh: unknown argument $1" >&2; exit 2 ;;
  esac
done
case "$PROFILE" in release|fast) ;; *) echo "install.sh: profile must be release or fast" >&2; exit 2 ;; esac

BINDIR=$PREFIX/bin
LIBEXECDIR=$PREFIX/libexec
DATADIR=$PREFIX/share
UNITDIR=$PREFIX/lib/systemd/user
# Binaries come from the Cargo target directory: $CARGO_TARGET_DIR when set (packagers), else target/ in the checkout.
OUT=${CARGO_TARGET_DIR:-$ROOT/target}/$PROFILE
D=$DESTDIR

# The session half: binaries (package -> binary), and the systemd user units made from tools/systemd/*.in.
SESSION_PKGS="helix-shell-tauri helix-shell-strut helix-wallpaper helix-idle helix-greeter helix-settings helix-mobius helix-software helix-system-monitor"
SESSION_BINS="helix-shell-tauri helix-shell-strut helix-wallpaper helix-idle helix-greeter helix-settings helix-mobius helix-software helix-system-monitor"
SESSION_UNITS="helix-shell-strut helix-shell-bar helix-shell-panels helix-idle helix-wallpaper"

if [ "$UNINSTALL" = 1 ]; then
  rm -f "$D$BINDIR"/helix-desktop "$D$BINDIR"/helix-assistant "$D$BINDIR"/helix-inference \
        "$D$BINDIR"/helix-transcribe "$D$BINDIR"/helix-image "$D$LIBEXECDIR/helix-system-upgrade" \
        "$D$UNITDIR"/helix-service.service "$D$UNITDIR"/helix-inference.service "$D$UNITDIR"/helix-assistant-warm.service \
        "$D$DATADIR"/dbus-1/services/org.helix.Desktop.service "$D$DATADIR"/dbus-1/services/org.helix.Inference.service \
        "$D$DATADIR"/polkit-1/actions/org.helix.Shell.upgrade.policy "$D$DATADIR"/icons/hicolor/scalable/apps/helix.svg \
        "$D$DATADIR"/helix/msg-risk.toml "$D$DATADIR"/helix/tool-hints.toml "$D$DATADIR"/helix/helix-wallpaper.png
  for bin in $SESSION_BINS helix-greeter-apply-appearance helix-panel helix-askpass helix-greeter-session; do rm -f "$D$BINDIR/$bin"; done
  for unit in $SESSION_UNITS; do rm -f "$D$UNITDIR/$unit.service"; done
  for app in Shell Settings Software SystemMonitor Mobius; do rm -f "$D$DATADIR/applications/org.helix.$app.desktop"; done
  rm -f "$D$DATADIR/icons/hicolor/scalable/apps/org.helix.Shell.svg" "$D$DATADIR/icons/hicolor/scalable/apps/org.helix.SystemMonitor.svg"
  rm -rf "$D$PREFIX/lib64/helix" "$D$DATADIR/helix/templates" "$D$DATADIR/helix-greeter"
  rmdir "$D$DATADIR/helix" 2>/dev/null || true
  echo "removed from ${D}${PREFIX}"
  exit 0
fi

if [ "$BUILD" = 1 ]; then
  command -v cargo >/dev/null || { echo "install.sh: cargo not found" >&2; exit 1; }
  packages="-p helix-desktop -p helix-assistant -p helix-inference -p helix-system-upgrade"
  if [ "$SESSION" = 1 ]; then for pkg in $SESSION_PKGS; do packages="$packages -p $pkg"; done; fi
  (cd "$ROOT" && cargo build --profile "$PROFILE" $packages)
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
# What the service reads from its assets root (<prefix>/share/helix is found by walking up from the executable):
# the theme templates and the wallpaper a first run starts with.
mkdir -p "$D$DATADIR/helix"
cp -r "$ROOT/assets/templates" "$D$DATADIR/helix/"
find "$D$DATADIR/helix/templates" -type d -exec chmod 0755 {} + -o -type f -exec chmod 0644 {} +
install -Dm644 "$ROOT/assets/helix-wallpaper.png" "$D$DATADIR/helix/helix-wallpaper.png"
install -Dm644 "$ROOT/protocols/ipc/tool-hints.toml" "$D$DATADIR/helix/tool-hints.toml"

# ---- the session half ------------------------------------------------------------------------------------------------------
if [ "$SESSION" = 1 ]; then
  have() { [ -x "$OUT/$1" ]; }
  missing=""
  for bin in $SESSION_BINS; do
    if have "$bin"; then install -Dm755 "$OUT/$bin" "$D$BINDIR/$bin"; else missing="$missing $bin"; fi
  done
  if [ -n "$missing" ]; then
    if [ "$REQUIRE" = 1 ]; then echo "install.sh: not built:$missing (build them, or drop --require-session)" >&2; exit 1; fi
    echo "note: not built, skipped:$missing" >&2
  fi
  # Settings finds the greeter appearance helper next to itself, so it goes in the same directory.
  if have helix-greeter-apply-appearance; then install -Dm755 "$OUT/helix-greeter-apply-appearance" "$D$BINDIR/helix-greeter-apply-appearance"; fi
  install -Dm755 "$ROOT/tools/helix-panel" "$D$BINDIR/helix-panel"
  install -Dm755 "$ROOT/tools/helix-askpass" "$D$BINDIR/helix-askpass"

  # Desktop entries and icons, only for what was installed.
  install_entry() { # binary entry-file
    [ -x "$D$BINDIR/$1" ] && install -Dm644 "$ROOT/assets/$2" "$D$DATADIR/applications/$2"
    return 0
  }
  install_entry helix-shell-tauri org.helix.Shell.desktop
  install_entry helix-settings org.helix.Settings.desktop
  install_entry helix-software org.helix.Software.desktop
  install_entry helix-system-monitor org.helix.SystemMonitor.desktop
  install_entry helix-mobius org.helix.Mobius.desktop
  [ -x "$D$BINDIR/helix-shell-tauri" ] && install -Dm644 "$ROOT/assets/org.helix.Shell.svg" "$D$DATADIR/icons/hicolor/scalable/apps/org.helix.Shell.svg"
  [ -x "$D$BINDIR/helix-system-monitor" ] && install -Dm644 "$ROOT/assets/org.helix.SystemMonitor.svg" "$D$DATADIR/icons/hicolor/scalable/apps/org.helix.SystemMonitor.svg"

  # systemd user units. Enabling is per user (`systemctl --user enable --now helix-shell-strut helix-shell-bar ...`).
  unit_args=("SHELL_BIN=$BINDIR/helix-shell-tauri" "STRUT_BIN=$BINDIR/helix-shell-strut" "IDLE_BIN=$BINDIR/helix-idle"
             "WALLPAPER_BIN=$BINDIR/helix-wallpaper" "ASKPASS=$BINDIR/helix-askpass")
  for unit in $SESSION_UNITS; do
    case $unit in
      helix-shell-strut) have_bin=helix-shell-strut ;;
      helix-shell-bar|helix-shell-panels) have_bin=helix-shell-tauri ;;
      helix-idle) have_bin=helix-idle ;;
      helix-wallpaper) have_bin=helix-wallpaper ;;
    esac
    [ -x "$D$BINDIR/$have_bin" ] && subst "$ROOT/tools/systemd/$unit.service.in" "$D$UNITDIR/$unit.service" "${unit_args[@]}"
  done

  # The greeter: binary, the session script greetd runs, and the private compositor's config. greetd's own configuration is
  # written by tools/install-greeter.sh, never by an install.
  if [ -x "$D$BINDIR/helix-greeter" ]; then
    install -Dm755 "$ROOT/greeter/session/helix-greeter-session" "$D$BINDIR/helix-greeter-session"
    install -Dm644 "$ROOT/greeter/session/greeter-niri.kdl" "$D$DATADIR/helix-greeter/niri.kdl"
  fi
fi

# The model engine's shared libraries, when it was built into third_party/llama-install.
ENGINE=$ROOT/third_party/llama-install/lib64
if [ -e "$ENGINE/libllama.so" ]; then
  PREFIX=$PREFIX DESTDIR=$DESTDIR python3 "$ROOT/tools/scripts/install_engine.py" "$ENGINE" lib64/helix
fi

echo "installed to ${D}${PREFIX}"
