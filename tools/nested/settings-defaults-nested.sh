#!/bin/bash
# Helix Settings against a small set of fixture applications (two browsers, a mail program, a file manager, a hidden one) and an existing mimeapps.list.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_SETTINGS_BIN:-$(ls -t "$ROOT/target/fast/helix-settings" "$ROOT/target/release/helix-settings" 2>/dev/null | head -1)}
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
SHARE=$XDG_STATE_HOME/fixture-share; mkdir -p "$SHARE/applications" "$XDG_DATA_HOME"
app() { printf '[Desktop Entry]\nType=Application\nName=%s\nExec=true\nMimeType=%s\n%s' "$2" "$3" "${4:-}" > "$SHARE/applications/$1.desktop"; }
app fox Fox "x-scheme-handler/https;x-scheme-handler/http;text/html;"
app owl Owl "x-scheme-handler/https;x-scheme-handler/http;"
app ghost Ghost "x-scheme-handler/https;" "NoDisplay=true"
app postbox Postbox "x-scheme-handler/mailto;"
app drawer Drawer "inode/directory;"
cat > "$SHARE/applications/mimeinfo.cache" <<'CACHE'
[MIME Cache]
x-scheme-handler/https=ghost.desktop;fox.desktop;owl.desktop;
x-scheme-handler/http=fox.desktop;owl.desktop;
text/html=fox.desktop;
x-scheme-handler/mailto=postbox.desktop;
inode/directory=drawer.desktop;
CACHE
export HELIX_SETTINGS_APPS_DIRS=$SHARE
mkdir -p "$XDG_CONFIG_HOME"
printf '# my own choices\n[Added Associations]\ntext/plain=editor.desktop;\n\n[Default Applications]\nx-scheme-handler/https=fox.desktop;\nimage/png=viewer.desktop;\n' > "$XDG_CONFIG_HOME/mimeapps.list"
python3 "$ROOT/tools/nested/fake-desktop.py" "$XDG_STATE_HOME" > "$XDG_STATE_HOME/fake-desktop.log" 2>&1 &
echo $! > "$XDG_STATE_HOME/helix-service.pid"
for _ in $(seq 1 50); do grep -q "ready" "$XDG_STATE_HOME/fake-desktop.log" 2>/dev/null && break; sleep 0.1; done
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
