#!/usr/bin/env bash
set -euo pipefail

theme_file="${XDG_CONFIG_HOME:-$HOME/.config}/Cursor/User/themes/noctalia.yaml"
db_file="${XDG_CONFIG_HOME:-$HOME/.config}/Cursor/User/globalStorage/state.vscdb"

rm -f -- "$theme_file"

if [[ ! -f "$db_file" ]]; then
    exit 0
fi

if ! command -v python3 >/dev/null 2>&1; then
    exit 0
fi

DB_FILE="$db_file" python3 - <<'PY'
import json
import os
import sqlite3
import sys

db_file = os.environ["DB_FILE"]
settings_prefix = "cursor-custom-theme:"
theme_id = "noctalia"
fallback = {
    "glass.theme.settingsId": "Cursor Dark",
    "glass.theme.darkSettingsId": "Cursor Dark",
    "glass.theme.lightSettingsId": "Cursor Light",
}

try:
    con = sqlite3.connect(db_file, timeout=5)
except sqlite3.Error:
    raise SystemExit(0)

try:
    row = con.execute("SELECT value FROM ItemTable WHERE key = ?", ("glass.theme.customThemesV1",)).fetchone()
    if row is None or not row[0]:
        raise SystemExit(0)
    try:
        parsed = json.loads(row[0])
    except json.JSONDecodeError:
        raise SystemExit(0)
    themes = parsed.get("themes") if isinstance(parsed, dict) else None
    if not isinstance(themes, dict):
        raise SystemExit(0)

    removed = []
    for key, value in list(themes.items()):
        name = value.get("name") if isinstance(value, dict) else None
        vscode_id = value.get("vscodeThemeSettingsId") if isinstance(value, dict) else None
        if key == theme_id or name in {"Helix", "Noctalia"} or vscode_id in {"Helix", "Noctalia"}:
            removed.append(key)
            themes.pop(key)

    if not removed:
        raise SystemExit(0)

    payload = json.dumps({"_v": 1, "themes": themes}, separators=(",", ":"))
    con.execute(
        "INSERT INTO ItemTable(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value",
        ("glass.theme.customThemesV1", payload),
    )
    removed_settings = {f"{settings_prefix}{key}" for key in removed}
    for key, default in fallback.items():
        current = con.execute("SELECT value FROM ItemTable WHERE key = ?", (key,)).fetchone()
        if current and current[0] in removed_settings:
            con.execute(
                "INSERT INTO ItemTable(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                (key, default),
            )
    con.commit()
except sqlite3.Error as exc:
    print(f"Warning: could not undo Cursor Agent theme state: {exc}", file=sys.stderr)
finally:
    con.close()
PY
