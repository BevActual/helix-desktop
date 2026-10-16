#!/usr/bin/env bash
# Formats (or with --check, verifies) the Rust the project owns with rustfmt's defaults.
# Left out on purpose: compositor/ (the niri fork keeps its own rustfmt.toml and tracks upstream) and third_party/.
#   tools/rustfmt.sh            rewrite files
#   tools/rustfmt.sh --check    fail on any difference (what CI runs)
set -euo pipefail
cd "$(dirname "$0")/.."
mapfile -t packages < <(cargo metadata --no-deps --format-version 1 --offline 2>/dev/null | python3 -c '
import json, os, sys
root = os.getcwd()
for p in json.load(sys.stdin)["packages"]:
    d = os.path.relpath(os.path.dirname(p["manifest_path"]), root)
    if not d.startswith(("compositor", "third_party")):
        print(p["name"])
')
args=()
for p in "${packages[@]}"; do args+=(-p "$p"); done
if [[ "${1:-}" == "--check" ]]; then
  cargo fmt "${args[@]}" -- --check
else
  cargo fmt "${args[@]}"
fi
