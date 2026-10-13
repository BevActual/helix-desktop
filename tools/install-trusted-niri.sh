#!/bin/bash
# Installs the Helix compositor build that protects the approval dialog (docs/design/trusted-surface.md) for the NEXT login, and turns it on in the
# person's niri config. Nothing here touches the running session: the compositor that is running keeps its own copy of the old binary.
#   tools/install-trusted-niri.sh apply     install the build (~/.local/bin/niri) and add the config; backups are made first
#   tools/install-trusted-niri.sh revert    put the previous binary and config back
#   tools/install-trusted-niri.sh status    what is installed now
# Applies to the user only (~/.local/bin, ~/.config/niri): no root, nothing under /usr.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NIRI_SRC=${HELIX_NIRI_SRC:-$ROOT/compositor}
NEW=$NIRI_SRC/target/release/niri
BIN=$HOME/.local/bin/niri
CONF_DIR=$HOME/.config/niri
CONFIG=$CONF_DIR/config.kdl
SNIPPET=$CONF_DIR/helix-trusted-surface.kdl
INCLUDE='include "helix-trusted-surface.kdl"'
STAMP=$(date +%Y%m%d-%H%M%S)

shell_exes() {  # every path the shell may run from, so the compositor can recognise it
  for candidate in "$ROOT/target/release/helix-shell-tauri" "$ROOT/target/fast/helix-shell-tauri" /usr/local/bin/helix-shell-tauri /usr/bin/helix-shell-tauri; do
    [ -e "$candidate" ] && readlink -f "$candidate"
  done | sort -u
}

write_snippet() {
  {
    echo "// Written by tools/install-trusted-niri.sh. The shell's approval dialog is a trusted surface: kept on top, capture-proof, closed to synthetic input."
    echo "trusted-surface {"
    echo '    namespace "helix-tauri-confirmer"'
    shell_exes | while read -r exe; do echo "    exe \"$exe\""; done
    echo "}"
  } > "$1"
}

case "${1:-status}" in
  apply)
    [ -x "$NEW" ] || { echo "no compositor build at $NEW (build it: cargo build --release --bin niri in $NIRI_SRC)"; exit 1; }
    "$NEW" --version | grep -q helix || { echo "$NEW is not the Helix build"; exit 1; }
    # The new build must accept the person's whole config, with the new block, before anything is installed.
    TMP=$(mktemp -d); cp -r "$CONF_DIR/." "$TMP/"
    write_snippet "$TMP/helix-trusted-surface.kdl"
    grep -qF "$INCLUDE" "$TMP/config.kdl" || printf '\n%s\n' "$INCLUDE" >> "$TMP/config.kdl"
    "$NEW" validate -c "$TMP/config.kdl" || { echo "the new build does not accept your config; nothing was changed"; rm -rf "$TMP"; exit 1; }
    rm -rf "$TMP"
    cp -p "$BIN" "$BIN.bak-helix-$STAMP"
    cp "$NEW" "$BIN.new" && chmod 755 "$BIN.new" && mv "$BIN.new" "$BIN"   # a new file: a running niri keeps its own copy
    cp -p "$CONFIG" "$CONFIG.bak-helix-$STAMP"
    write_snippet "$SNIPPET"
    grep -qF "$INCLUDE" "$CONFIG" || printf '\n%s\n' "$INCLUDE" >> "$CONFIG"
    echo "installed $("$BIN" --version); previous binary: $BIN.bak-helix-$STAMP; config backup: $CONFIG.bak-helix-$STAMP"
    echo "The running session is unchanged (it may show a config error until you log out: the old compositor does not know the new block)."
    echo "Log out and back in to use it. If the desktop does not start: from a TTY run  tools/install-trusted-niri.sh revert"
    ;;
  revert)
    last=$(ls -t "$BIN".bak-helix-* 2>/dev/null | head -1 || true)
    [ -n "$last" ] || { echo "no backup binary found"; exit 1; }
    cp "$last" "$BIN.new" && chmod 755 "$BIN.new" && mv "$BIN.new" "$BIN"
    if [ -f "$CONFIG" ]; then grep -vF "$INCLUDE" "$CONFIG" > "$CONFIG.new" && mv "$CONFIG.new" "$CONFIG"; fi
    rm -f "$SNIPPET"
    echo "restored $last and removed the trusted-surface include"
    ;;
  status)
    echo "binary: $("$BIN" --version 2>&1 | head -1)"
    ls "$BIN".bak-helix-* 2>/dev/null | sed 's/^/backup: /' || true
    [ -f "$SNIPPET" ] && { echo "config snippet:"; sed 's/^/  /' "$SNIPPET"; } || echo "config snippet: none"
    grep -qF "$INCLUDE" "$CONFIG" && echo "included in config.kdl: yes" || echo "included in config.kdl: no"
    running=$(pgrep -x niri | head -1)
    if [ -n "$running" ]; then
      run_exe=$(readlink -f "/proc/$running/exe" 2>/dev/null || echo unknown)
      echo "running compositor: $run_exe"
      case "$run_exe" in
        *\(deleted\)*)
          echo "note: the running compositor still has the old binary mapped — log out and back in to use $BIN"
          ;;
      esac
      if [ -x "$BIN" ] && [ -f "$run_exe" ] && [ "$run_exe" != "$(readlink -f "$BIN" 2>/dev/null)" ]; then
        echo "note: running binary differs from $BIN — log out and back in to pick up the installed build"
      fi
    else
      echo "running compositor: none"
    fi
    ;;
  *) sed -n 2,9p "$0"; exit 2 ;;
esac
