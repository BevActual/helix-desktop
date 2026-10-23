#!/bin/bash
# Helix Settings against fixture autostart folders (a system keyring entry, a system one already off for the person, one the person added) and fixture
# applications to add from.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_SETTINGS_BIN:-$(ls -t "$ROOT/target/fast/helix-settings" "$ROOT/target/release/helix-settings" 2>/dev/null | head -1)}
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
SYS=$XDG_STATE_HOME/fixture-system-autostart; SHARE=$XDG_STATE_HOME/fixture-share
mkdir -p "$SYS" "$SHARE/applications" "$XDG_CONFIG_HOME/autostart"
printf '[Desktop Entry]\nType=Application\nName=Keyring\nComment=Keeps your passwords unlocked.\nExec=true\n' > "$SYS/keyring.desktop"
printf '[Desktop Entry]\nType=Application\nName=Printer applet\nExec=true\n' > "$SYS/printer.desktop"
printf '[Desktop Entry]\nType=Application\nName=Printer applet\nHidden=true\n' > "$XDG_CONFIG_HOME/autostart/printer.desktop"
printf '[Desktop Entry]\nType=Application\nName=My sync\nExec=true\n' > "$XDG_CONFIG_HOME/autostart/mysync.desktop"
app() { printf '[Desktop Entry]\nType=Application\nName=%s\nExec=true\n' "$2" > "$SHARE/applications/$1.desktop"; }
app notes Notes; app clock Clockwork; app mysync "My sync"
export HELIX_SETTINGS_AUTOSTART_SYSTEM_DIRS=$SYS HELIX_SETTINGS_APPS_DIRS=$SHARE
python3 "$ROOT/tools/nested/fake-desktop.py" "$XDG_STATE_HOME" > "$XDG_STATE_HOME/fake-desktop.log" 2>&1 &
echo $! > "$XDG_STATE_HOME/helix-service.pid"
for _ in $(seq 1 50); do grep -q "ready" "$XDG_STATE_HOME/fake-desktop.log" 2>/dev/null && break; sleep 0.1; done
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
