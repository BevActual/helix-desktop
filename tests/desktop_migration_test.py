"""Keep every existing feature accounted for until the desktop cutover."""
import json
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
inventory = json.loads((root / "docs/design/desktop-migration.json").read_text())
entries = inventory["entries"]
assert len({(entry["kind"], entry["id"]) for entry in entries}) == len(entries)
control = (root / "src/shell/control_center/control_center_panel.cpp").read_text()
tabs = set()
for name in ("kBuiltinTabs", "kHubChildren"):
    table = control.split(name + "[] = {")[1].split("\n  };")[0]
    tabs.update(re.findall(r'\{"([^\"]+)"', table))
sections = set(re.findall(r'\{SettingsSection::\w+, "([^\"]+)"',
    (root / "src/shell/settings/settings_registry.cpp").read_text()))
assert {entry["id"] for entry in entries if entry["kind"] == "tab"} == tabs
assert {entry["id"] for entry in entries if entry["kind"] == "settings-section"} == sections
assert all(entry["destination"] and entry["status"] in {"pending", "partial", "verified"} for entry in entries)
shortcut_registry = (root / "src/shell/control_center/shortcut_registry.cpp").read_text()
builtins = shortcut_registry.split("kBuiltinShortcuts =")[1].split("\n  });")[0]
shortcut_metadata = (root / "src/desktop/shortcuts.h").read_text().split("builtInShortcuts[] = {")[1].split("\n};")[0]
assert set(re.findall(r'\.type = "([^\"]+)"', builtins)) == set(re.findall(r'\{"([^\"]+)"', shortcut_metadata)), "Service shortcut catalog must match the shell"
if inventory["cutover"]:
    assert all(entry["status"] == "verified" for entry in entries), "Cutover requires destination parity"
