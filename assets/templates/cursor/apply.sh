#!/usr/bin/env bash
set -euo pipefail

theme_file="${XDG_CONFIG_HOME:-$HOME/.config}/Cursor/User/themes/noctalia.yaml"
db_file="${XDG_CONFIG_HOME:-$HOME/.config}/Cursor/User/globalStorage/state.vscdb"

if [[ ! -f "$theme_file" ]]; then
    printf 'Warning: Cursor Agent theme file not found: %s\n' "$theme_file" >&2
    exit 0
fi

if [[ ! -f "$db_file" ]]; then
    exit 0
fi

if ! command -v python3 >/dev/null 2>&1; then
    printf 'Warning: python3 is required to apply the Cursor Agent theme to running Cursor state.\n' >&2
    exit 0
fi

THEME_FILE="$theme_file" DB_FILE="$db_file" python3 - <<'PY'
import json
import os
import sqlite3
import sys
import time

theme_file = os.environ["THEME_FILE"]
db_file = os.environ["DB_FILE"]
theme_id = "noctalia"

def parse_flat_yaml(path: str) -> dict:
    data: dict = {}
    with open(path, encoding="utf-8") as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith("#") or ":" not in line:
                continue
            key, value = line.split(":", 1)
            key = key.strip()
            value = value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
                value = value[1:-1]
            lowered = value.lower()
            if lowered == "true":
                data[key] = True
            elif lowered == "false":
                data[key] = False
            else:
                data[key] = value
    return data

definition = parse_flat_yaml(theme_file)
if not isinstance(definition.get("name"), str) or not definition["name"]:
    print("Warning: Cursor Agent theme is missing a name", file=sys.stderr)
    raise SystemExit(0)

now = int(time.time() * 1000)
record = {
    "name": definition["name"],
    "source": "imported",
    "definition": definition,
    "vscodeThemeSettingsId": "Helix",
    "createdAt": now,
    "updatedAt": now,
}

try:
    con = sqlite3.connect(db_file, timeout=5)
except sqlite3.Error as exc:
    print(f"Warning: could not open Cursor state database: {exc}", file=sys.stderr)
    raise SystemExit(0)

try:
    row = con.execute("SELECT value FROM ItemTable WHERE key = ?", ("glass.theme.customThemesV1",)).fetchone()
    themes: dict = {}
    if row and row[0]:
        try:
            parsed = json.loads(row[0])
            if isinstance(parsed, dict) and isinstance(parsed.get("themes"), dict):
                themes = parsed["themes"]
        except json.JSONDecodeError:
            themes = {}

    existing_id = theme_id
    existing = themes.get(theme_id) if isinstance(themes.get(theme_id), dict) else None
    if existing is None:
        for key, value in themes.items():
            if not isinstance(value, dict):
                continue
            if value.get("name") in {"Helix", "Noctalia"} or value.get("vscodeThemeSettingsId") in {"Helix", "Noctalia"}:
                existing_id = key
                existing = value
                break
    if isinstance(existing, dict) and isinstance(existing.get("createdAt"), (int, float)):
        record["createdAt"] = int(existing["createdAt"])

    if existing is None and len(themes) >= 30:
        print("Warning: Cursor already has 30 custom Agent themes; delete one, then re-apply", file=sys.stderr)
        raise SystemExit(0)

    themes[existing_id] = record
    settings_id = f"cursor-custom-theme:{existing_id}"
    payload = json.dumps({"_v": 1, "themes": themes}, separators=(",", ":"))
    con.execute(
        "INSERT INTO ItemTable(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value",
        ("glass.theme.customThemesV1", payload),
    )
    for key in (
        "glass.theme.settingsId",
        "glass.theme.darkSettingsId",
        "glass.theme.lightSettingsId",
    ):
        con.execute(
            "INSERT INTO ItemTable(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value",
            (key, settings_id),
        )
    con.commit()
except sqlite3.Error as exc:
    print(f"Warning: could not update Cursor Agent theme state: {exc}", file=sys.stderr)
finally:
    con.close()
PY
