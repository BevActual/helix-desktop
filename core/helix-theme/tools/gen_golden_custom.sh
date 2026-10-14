#!/usr/bin/env bash
# Records helix-theme-palette (C++ reference) results for the scenarios below into
# core/helix-theme/tests/golden/custom/. Run from the repo root after building the helper:
#   meson compile -C build-debug helix-theme-palette
set -euo pipefail
bin=${HELIX_THEME_PALETTE_BIN:-build-debug/helix-theme-palette}
dest=core/helix-theme/tests/golden/custom
rm -rf "$dest"
mkdir -p "$dest"

# name | [theme] body | steps (';' separated) | pre-existing palette to copy (optional)
scenarios=(
  'builtin-dark-accent|source = "builtin"\nbuiltin = "Catppuccin"\nmode = "dark"|set-accent #ff8800|'
  'builtin-light-accent|source = "builtin"\nbuiltin = "Gruvbox"\nmode = "light"|set-accent #12ab34|'
  'default-theme-accent|mode = "dark"|set-accent #3366cc|'
  'unknown-builtin|source = "builtin"\nbuiltin = "NoSuchPalette"\nmode = "dark"|set-role 4 #aa22cc|'
  'chained-roles|source = "builtin"\nbuiltin = "Dracula"\nmode = "dark"|set-accent #ff0000;set-role 2 #00ff00;set-role 8 #101820;set-role 6 #ffcc00;set-role 12 #778899|'
  'every-role|source = "builtin"\nbuiltin = "Eldritch"\nmode = "dark"|set-role 0 #204060;set-role 1 #204060;set-role 2 #204060;set-role 3 #204060;set-role 4 #204060;set-role 5 #204060;set-role 6 #204060;set-role 7 #204060;set-role 8 #204060;set-role 9 #204060;set-role 10 #204060;set-role 11 #204060;set-role 12 #204060;set-role 13 #204060;set-role 14 #204060;set-role 15 #204060|'
  'save-current-builtin|source = "builtin"\nbuiltin = "Kanagawa"\nmode = "light"|save-palette current|'
  'save-then-edit|source = "builtin"\nbuiltin = "Ayu"\nmode = "dark"|save-palette current;set-accent #e0a030;save-palette current|'
  'name-collision|source = "builtin"\nbuiltin = "Helix"\nmode = "dark"|set-accent #ff00ff|custom.json'
)

for entry in "${scenarios[@]}"; do
  IFS='|' read -r name body steps pre <<<"$entry"
  sdir=$dest/$name
  work=$(mktemp -d)
  mkdir -p "$sdir" "$work/cfg/helix/palettes" "$work/state" "$work/data" "$work/cache"
  printf "[theme]\n$body\n" >"$sdir/config.toml"
  cp "$sdir/config.toml" "$work/cfg/helix/config.toml"
  if [ -n "$pre" ]; then
    mkdir -p "$sdir/pre"
    cp core/helix-theme/tests/golden/pre/"$pre" "$sdir/pre/$pre"
    cp "$sdir/pre/$pre" "$work/cfg/helix/palettes/$pre"
  fi
  : >"$sdir/steps"
  : >"$sdir/expected_steps"
  IFS=';' read -ra list <<<"$steps"
  for step in "${list[@]}"; do
    echo "$step" >>"$sdir/steps"
    set +e
    out=$(env HOME="$work" XDG_CONFIG_HOME="$work/cfg" XDG_STATE_HOME="$work/state" \
      XDG_DATA_HOME="$work/data" XDG_CACHE_HOME="$work/cache" \
      "$bin" ${step%% *} ${step#* } 2>/dev/null)
    code=$?
    set -e
    echo "$code $out" >>"$sdir/expected_steps"
  done
  mkdir -p "$sdir/expected"
  cp "$work"/cfg/helix/palettes/*.json "$sdir/expected/" 2>/dev/null || true
  # Selection after the run, read back from the overrides the helper wrote.
  cp "$work/state/helix/settings.toml" "$sdir/expected_settings.toml" 2>/dev/null || true
  rm -rf "$work"
done
