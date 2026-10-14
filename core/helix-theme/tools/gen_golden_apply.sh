#!/usr/bin/env bash
# Records helix-template-apply (C++ reference) results for tests/golden/apply/*.
# Needs: meson compile -C build-debug helix-template-apply. Run from the repo root.
set -euo pipefail
bin=${HELIX_TEMPLATE_APPLY_BIN:-build-debug/helix-template-apply}
root=/tmp/helix-golden-apply-root
dest=core/helix-theme/tests/golden/apply

for scenario in "$dest"/*/; do
  name=$(basename "$scenario")
  rm -rf "$root"
  mkdir -p "$root/cfg/helix" "$root/state/helix" "$root/data" "$root/cache"
  cp "$scenario/config.toml" "$root/cfg/helix/config.toml"
  [ -d "$scenario/files" ] && cp -a "$scenario/files/." "$root/cfg/"
  [ -d "$scenario/statefiles" ] && cp -a "$scenario/statefiles/." "$root/state/helix/"
  [ -f "$scenario/state.toml" ] && cp "$scenario/state.toml" "$root/state/helix/state.toml"

  set +e
  env HOME="$root" XDG_CONFIG_HOME="$root/cfg" XDG_STATE_HOME="$root/state" XDG_DATA_HOME="$root/data" \
    XDG_CACHE_HOME="$root/cache" HELIX_ASSETS_DIR="$PWD/assets" \
    "$bin" >"$root/stdout" 2>"$root/stderr"
  code=$?
  set -e

  out="$scenario/expected"
  rm -rf "$out"
  mkdir -p "$out"
  echo "$code" >"$out/exit_code"
  grep '^template failure: ' "$root/stderr" | sort >"$out/failures.txt" || true
  # Snapshot every file the run left behind; the root path is normalised to @ROOT@.
  (cd "$root" && find . -type f ! -name stdout ! -name stderr | sed 's|^\./||' | sort) | while read -r rel; do
    mkdir -p "$out/tree/$(dirname "$rel")"
    sed "s|$root|@ROOT@|g" "$root/$rel" >"$out/tree/$rel"
  done
done
