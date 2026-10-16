#!/usr/bin/env python3
"""Layering check for the Cargo workspace: new upward or sideways path dependencies fail, and the
recorded baseline may only shrink.

  tools/check-layers.py <repo-root>                    check (exit 1 on a new violation or a stale baseline entry)
  tools/check-layers.py <repo-root> --update-baseline  rewrite the baseline from the current tree
  tools/check-layers.py <repo-root> --list             print current violations

Rules (directories, and the order of crates inside core/) live in tools/layers.toml; known exceptions in tools/layers-baseline.txt (one `crate -> dependency` per line).
"""
import json
import os
import pathlib
import subprocess
import sys
import tomllib


def top_dir(rel):
    return rel.split("/", 1)[0]


def violations(root):
    config = tomllib.loads((root / "tools/layers.toml").read_text())
    rules = config["rules"]
    crate_rules = config.get("crates", {})
    out = subprocess.run(
        ["cargo", "metadata", "--format-version", "1", "--no-deps", "--offline"],
        cwd=root, capture_output=True, text=True,
    )
    if out.returncode != 0:
        sys.exit(f"cargo metadata failed:\n{out.stderr}")
    metadata = json.loads(out.stdout)
    base = metadata["workspace_root"]
    where = {
        package["name"]: os.path.relpath(os.path.dirname(package["manifest_path"]), base)
        for package in metadata["packages"]
    }
    found = {}
    for package in metadata["packages"]:
        here = where[package["name"]]
        allowed = rules.get(top_dir(here))
        if allowed is None:
            found[f"{here} -> (no rule for this directory)"] = (here, "")
            continue
        for dep in package["dependencies"]:
            if not dep.get("path") or dep["name"] not in where:
                continue
            there = where[dep["name"]]
            if top_dir(there) not in allowed:
                found[f"{package['name']} -> {dep['name']}"] = (here, there)
            elif top_dir(here) == "core" and top_dir(there) == "core":
                # Inside core/ the order of crates matters, not just the directory.
                if package["name"] not in crate_rules:
                    found[f"{package['name']} has no entry in [crates] of tools/layers.toml"] = (here, there)
                elif dep["name"] not in crate_rules[package["name"]]:
                    found[f"{package['name']} -> {dep['name']}"] = (here, there)
    return found


def main():
    args = [arg for arg in sys.argv[1:] if not arg.startswith("--")]
    flags = {arg for arg in sys.argv[1:] if arg.startswith("--")}
    root = pathlib.Path(args[0] if args else ".").resolve()
    baseline_path = root / "tools/layers-baseline.txt"
    found = violations(root)

    if "--list" in flags:
        for key, (here, there) in sorted(found.items()):
            print(f"{key}    ({here} -> {there})")
        return 0
    if "--update-baseline" in flags:
        baseline_path.write_text("".join(f"{key}\n" for key in sorted(found)))
        print(f"baseline: {len(found)} entries")
        return 0

    baseline = set(baseline_path.read_text().split("\n")) - {""} if baseline_path.exists() else set()
    new = sorted(set(found) - baseline)
    stale = sorted(baseline - set(found))
    for key in new:
        print(f"NEW violation: {key}")
    for key in stale:
        print(f"STALE baseline entry (fixed — remove it with --update-baseline): {key}")
    if new or stale:
        return 1
    print(f"layering ok ({len(found)} known exceptions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
