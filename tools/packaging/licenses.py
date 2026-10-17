#!/usr/bin/env python3
"""The license expression of what a package contains, from Cargo metadata (the crates linked into it) and package-lock.json (the JavaScript
bundled into the web views). Packages in the workspace itself are MIT, except the compositor (GPL-3.0-or-later).

  tools/packaging/licenses.py                 print the expression for each package group
  tools/packaging/licenses.py --check SPEC    fail if the License: lines of SPEC differ from these
  tools/packaging/licenses.py --write SPEC    rewrite the License: lines of SPEC

A crate's own `A/B` (the old Cargo way of saying OR) is written `A OR B`. Each distinct expression is joined with AND, which is how Fedora asks
for the license of a package with bundled code."""
import json, re, subprocess, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
HELIX = ["helix-desktop", "helix-assistant", "helix-inference", "helix-system-upgrade", "helix-shell-tauri", "helix-shell-strut",
         "helix-wallpaper", "helix-idle", "helix-settings", "helix-mobius", "helix-software", "helix-system-monitor"]
GROUPS = {"helix": HELIX, "helix-niri": ["niri"], "helix-greeter": ["helix-greeter"]}
OWN = {"helix": "MIT", "helix-niri": "GPL-3.0-or-later", "helix-greeter": "MIT"}

def norm(expr: str) -> str:
    expr = re.sub(r"\s*/\s*", " OR ", expr.strip())
    return re.sub(r"\s+", " ", expr)

def rust_licenses(roots):
    meta = json.loads(subprocess.check_output(["cargo", "metadata", "--format-version", "1", "--offline"], cwd=ROOT, stderr=subprocess.DEVNULL))
    packages = {p["id"]: p for p in meta["packages"]}
    workspace = set(meta["workspace_members"])
    nodes = {n["id"]: n for n in meta["resolve"]["nodes"]}
    by_name = {packages[i]["name"]: i for i in workspace}
    seen, stack = set(), [by_name[r] for r in roots]
    while stack:
        i = stack.pop()
        if i in seen:
            continue
        seen.add(i)
        for d in nodes[i]["deps"]:
            if any(k["kind"] in (None, "build") for k in d["dep_kinds"]):
                stack.append(d["pkg"])
    found, missing = set(), []
    for i in seen:
        if i in workspace:
            continue
        lic = packages[i].get("license")
        (found.add(norm(lic)) if lic else missing.append(packages[i]["name"]))
    return found, missing

def npm_licenses():
    lock = json.loads((ROOT / "package-lock.json").read_text())
    found, missing = set(), []
    for path, entry in lock.get("packages", {}).items():
        # The workspace's own web packages (no node_modules in the path) are ours, MIT.
        if "node_modules/" not in path or entry.get("dev") or entry.get("link"):
            continue
        lic = entry.get("license")
        if isinstance(lic, dict):
            lic = lic.get("type")
        (found.add(norm(lic)) if lic else missing.append(path))
    return found, missing

def expression(group):
    found, missing = rust_licenses(GROUPS[group])
    if group in ("helix", "helix-greeter"):
        js, js_missing = npm_licenses()
        found |= js
        missing += js_missing
    found.add(OWN[group])
    if missing:
        sys.exit(f"{group}: no license declared for: {', '.join(sorted(missing))}")
    # Parenthesize anything that is itself compound so AND binds as written.
    parts = sorted(f"({x})" if re.search(r"\b(OR|AND|WITH)\b", x) else x for x in found)
    return " AND ".join(parts)

def spec_licenses(path):
    """License: lines in order: main package, then each subpackage."""
    return [m.group(1).strip() for m in re.finditer(r"^License:\s*(.+)$", pathlib.Path(path).read_text(), re.M)]

if __name__ == "__main__":
    want = {g: expression(g) for g in GROUPS}
    if len(sys.argv) == 3 and sys.argv[1] == "--write":
        text = pathlib.Path(sys.argv[2]).read_text()
        expected = iter([want["helix"], want["helix-niri"], want["helix-greeter"]])
        text = re.sub(r"^(License:\s*).+$", lambda m: m.group(1) + next(expected), text, flags=re.M)
        pathlib.Path(sys.argv[2]).write_text(text)
        print("licenses written")
    elif len(sys.argv) == 3 and sys.argv[1] == "--check":
        have = spec_licenses(sys.argv[2])
        expected = [want["helix"], want["helix-niri"], want["helix-greeter"]]
        bad = [g for g, h, e in zip(GROUPS, have, expected) if h != e]
        if len(have) != 3 or bad:
            sys.exit(f"the License: lines of {sys.argv[2]} are out of date ({', '.join(bad) or 'wrong count'}); run tools/packaging/licenses.py")
        print("licenses ok")
    else:
        for g, e in want.items():
            print(f"{g}: {e}\n")
