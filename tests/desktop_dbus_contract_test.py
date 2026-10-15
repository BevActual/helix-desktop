"""Structural rules for the D-Bus contracts in protocols/dbus/ (the Rust catalog check lives in helix-desktop-api)."""
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

root = pathlib.Path(sys.argv[1])
def load(path):
    return {i.get("name"): i for i in ET.parse(path).getroot().findall("interface")}


interfaces = load(root / "protocols/dbus/org.helix.Desktop.xml")
drafts = {}
for draft in sorted((root / "protocols/dbus/drafts").glob("*.xml")):
    drafts.update(load(draft))
assert not set(drafts) & set(interfaces), "an interface cannot be both draft and registered"
# Other services' contracts (one file per bus name) get the same structural rules.
others = load(root / "protocols/dbus/org.helix.Inference.xml")
assert set(others) == {"org.helix.Inference1"}, others

for name, node in {**interfaces, **drafts, **others}.items():
    assert re.fullmatch(r"org\.helix\.(Desktop\.)?[A-Z][A-Za-z]*\d+", name), f"{name}: needs a versioned name"
    methods = [m.get("name") for m in node.findall("method")]
    assert len(methods) == len(set(methods)), f"{name}: duplicate method"
    if "Action" in methods:  # async-operation interfaces must be cancellable and report completion
        assert "Cancel" in methods, f"{name}: Action without Cancel"
        assert "Completed" in [s.get("name") for s in node.findall("signal")], f"{name}: Action without Completed"
    for method in node.findall("method"):
        for arg in method.findall("arg"):
            assert arg.get("type") and arg.get("direction") in {"in", "out"}, f"{name}.{method.get('name')}: bad arg"
