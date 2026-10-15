#!/bin/sh
# Installs the model runtime and the assistant into /usr/local from a staged `meson install --destdir` tree.
#   meson install -C build-debug --destdir "$STAGE" --no-rebuild && sudo tools/install-assistant.sh "$STAGE"
# Only these pieces are touched (the shell and the other apps are left alone): helix-inference,
# helix-assistant, the Tauri Settings app when staged as bin/helix-settings-tauri (it becomes /usr/local/bin/helix-settings; the one it
# replaces is backed up), the engine libraries in /usr/local/lib64/helix, the policy and hints files, the two D-Bus service
# files and the user units. Files it replaces are first copied to /usr/local/share/helix/backup/<timestamp>/.
# It does not restart anything. To undo: tools/install-assistant.sh --uninstall
set -eu
P=/usr/local
if [ "${1:-}" = "--uninstall" ]; then
  rm -f $P/bin/helix-inference $P/bin/helix-assistant $P/share/helix/tool-hints.toml \
        $P/share/dbus-1/services/org.helix.Inference.service \
        $P/lib/systemd/user/helix-inference.service $P/lib/systemd/user/helix-assistant-warm.service
  rm -rf $P/lib64/helix
  echo "Removed. Replaced files were not rolled back; see $P/share/helix/backup/ for the previous copy."
  exit 0
fi
STAGE=${1:?usage: install-assistant.sh <staged install dir>}
S=$STAGE$P
[ -d "$S/lib64/helix" ] || { echo "not a staged install: $STAGE" >&2; exit 1; }
BACKUP=$P/share/helix/backup/$(date +%Y%m%d-%H%M%S)
mkdir -p "$BACKUP"
INFERENCE=${HELIX_INFERENCE_BIN:-$(dirname "$0")/../target/release/helix-inference}   # Rust: cargo build --release -p helix-inference
[ -x "$INFERENCE" ] || { echo "missing $INFERENCE" >&2; exit 1; }
install -D -m 0755 "$INFERENCE" $P/bin/helix-inference
ASSISTANT=${HELIX_ASSISTANT_BIN:-$(dirname "$0")/../target/release/helix-assistant}   # Rust: cargo build --release -p helix-assistant
[ -x "$ASSISTANT" ] || { echo "missing $ASSISTANT" >&2; exit 1; }
install -D -m 0755 "$ASSISTANT" $P/bin/helix-assistant
if [ -x "$S/bin/helix-settings-tauri" ]; then
  [ -e $P/bin/helix-settings ] && cp -p $P/bin/helix-settings "$BACKUP/helix-settings"
  install -D -m 0755 "$S/bin/helix-settings-tauri" $P/bin/helix-settings.new && mv -f $P/bin/helix-settings.new $P/bin/helix-settings
  echo "Settings app replaced (the previous one is in $BACKUP/helix-settings)."
fi
mkdir -p $P/lib64/helix
cp -a "$S"/lib64/helix/. $P/lib64/helix/
for f in share/helix/msg-risk.toml share/helix/tool-hints.toml share/dbus-1/services/org.helix.Inference.service \
         lib/systemd/user/helix-inference.service lib/systemd/user/helix-assistant-warm.service; do
  install -D -m 0644 "$S/$f" "$P/$f"
done
echo "Installed. Backup of replaced files: $BACKUP"
# A running helix-inference keeps its old binary but loads the engine libraries at the next model load: after the libraries were replaced that is
# a mismatch, and it crashed (SIGSEGV in LlamaBackend::load, 2026-10-07). So the person's own copies of the units are restarted now.
if [ -n "${SUDO_USER:-}" ] && command -v runuser >/dev/null 2>&1; then
  uid=$(id -u "$SUDO_USER")
  if [ -d "/run/user/$uid" ]; then
    runuser -u "$SUDO_USER" -- env XDG_RUNTIME_DIR="/run/user/$uid" systemctl --user try-restart helix-inference.service helix-service.service \
      && echo "Restarted helix-inference and helix-service for $SUDO_USER." || echo "Could not restart them: run  systemctl --user restart helix-inference helix-service"
  fi
else
  echo "Restart them to use the new files:  systemctl --user restart helix-inference helix-service"
fi
