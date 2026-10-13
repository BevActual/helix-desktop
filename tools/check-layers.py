#!/usr/bin/env python3
"""Layering check for src/: new upward includes fail, the recorded baseline may only shrink.

  tools/check-layers.py <repo-root>                  check (exit 1 on a new violation or a stale baseline entry)
  tools/check-layers.py <repo-root> --update-baseline  rewrite the baseline from the current tree
  tools/check-layers.py <repo-root> --list           print current violations grouped by module pair
"""
import collections
import pathlib
import re
import sys
import tomllib

INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def load(root):
    config = tomllib.loads((root / "tools/layers.toml").read_text())
    layer_of, rank = {}, {}
    for index, (name, modules) in enumerate(config["layers"].items()):
        rank[name] = index
        for module in modules:
            layer_of[module] = name
    independent = set(config["independent"]["modules"])
    allowed = set(config["independent"]["may_include"])
    for module in independent:
        layer_of[module] = "independent"
    return layer_of, rank, independent, allowed


def violations(root):
    layer_of, rank, independent, allowed = load(root)
    found = {}
    src = root / "src"
    for path in sorted(src.rglob("*")):
        if path.suffix not in (".h", ".cpp"):
            continue
        parts = path.relative_to(src).parts
        if len(parts) < 2 or parts[0] not in layer_of:
            continue
        module = parts[0]
        for include in INCLUDE.findall(path.read_text(errors="ignore")):
            target = include.split("/")
            if len(target) < 2 or target[0] not in layer_of or target[0] == module:
                continue
            here, there = layer_of[module], layer_of[target[0]]
            if here == "independent":
                bad = there not in allowed
            elif there == "independent":
                bad = True  # nothing outside the independent processes may depend on them
            else:
                bad = rank[there] > rank[here]
            if bad:
                found[f"src/{'/'.join(parts)} -> {include}"] = (module, target[0])
    return found


def main():
    root = pathlib.Path(sys.argv[1]).resolve()
    flags = set(sys.argv[2:])
    baseline_file = root / "tools/layers-baseline.txt"
    current = violations(root)

    if "--update-baseline" in flags:
        baseline_file.write_text("".join(line + "\n" for line in sorted(current)))
        print(f"baseline written: {len(current)} entries")
        return 0
    if "--list" in flags:
        pairs = collections.Counter(current.values())
        for (a, b), count in sorted(pairs.items(), key=lambda item: -item[1]):
            print(f"{count:3d}  {a} -> {b}")
        print(f"{len(current)} violations")
        return 0

    baseline = set(baseline_file.read_text().splitlines()) if baseline_file.exists() else set()
    new = sorted(set(current) - baseline)
    fixed = sorted(baseline - set(current))
    status = 0
    if new:
        print("New layering violations (a module includes something from a higher layer, see tools/layers.toml):")
        for line in new:
            print(f"  {line}")
        print("Fix the dependency (move the shared piece down a layer, or pass it in) instead of adding it to the baseline.")
        status = 1
    if fixed:
        print("These baseline entries no longer exist. Remove them from tools/layers-baseline.txt "
              "(or run with --update-baseline):")
        for line in fixed:
            print(f"  {line}")
        status = 1
    if status == 0:
        print(f"layering ok: {len(current)} known violations, none new")
    return status


sys.exit(main())
