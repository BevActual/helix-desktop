"""Keep the desktop migration inventory well-formed until the desktop cutover."""
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
inventory = json.loads((root / "docs/design/archive/desktop-migration.json").read_text())
entries = inventory["entries"]
assert len({(entry["kind"], entry["id"]) for entry in entries}) == len(entries)
assert all(entry["destination"] and entry["status"] in {"pending", "partial", "verified"} for entry in entries)
if inventory["cutover"]:
    assert all(entry["status"] == "verified" for entry in entries), "Cutover requires destination parity"
