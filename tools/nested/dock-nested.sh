#!/bin/bash
# Started by run.sh: two fake applications (their windows announce the app ids fakeone and faketwo) and a dock that pins both, then the
# shell against the fake desktop service. Nothing real is launched.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$XDG_DATA_HOME/applications" "$XDG_CONFIG_HOME/helix"
for name in one two; do
  # Each fake application is a script named for it (as a real one would be): the dock checks the program a window really belongs to.
  ln -sf "$ROOT/tools/nested/text-target.py" "$XDG_STATE_HOME/fake$name.py"
  cat > "$XDG_DATA_HOME/applications/fake$name.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=Fake ${name^}
Exec=env APP_ID=fake$name TARGET_FILE=$XDG_STATE_HOME/fake$name.txt python3 "$XDG_STATE_HOME/fake$name.py"
Icon=
DESK
done
printf '{"version":1,"pinned":["faketwo.desktop","fakeone.desktop"]}' > "$XDG_CONFIG_HOME/helix/dock.json"
exec "$ROOT/tools/nested/tauri-fake-desktop.sh"
