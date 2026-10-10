#!/bin/bash
# One-time, additive move of the old product's settings into Helix's folders (the C++ rename changed where the service reads them):
#   ~/.config/noctalia  -> ~/.config/helix        ~/.local/state/noctalia -> ~/.local/state/helix        (same for data and cache)
# Nothing is overwritten and nothing is deleted: a file Helix already has wins, and the old folder stays as it was (a backup).
# Agent grants and the audit (state/agent) are NOT carried over: access given to programs is something to say yes to again.
#   tools/migrate-state.sh [--dry-run]
set -eu
dry=0; [ "${1:-}" = "--dry-run" ] && dry=1
home=${HOME:?}
copied=0; kept=0
migrate() {  # old new
  local old=$1 new=$2
  [ -d "$old" ] || return 0
  while IFS= read -r -d '' path; do
    local rel=${path#"$old"/} target
    target="$new/$rel"
    if [ -d "$path" ] && [ ! -L "$path" ]; then continue; fi
    case "$rel" in agent/*) continue ;; esac
    if [ -e "$target" ] || [ -L "$target" ]; then kept=$((kept + 1)); continue; fi
    echo "  $rel"
    copied=$((copied + 1))
    [ $dry = 1 ] && continue
    mkdir -p "$(dirname "$target")"
    cp -p --no-clobber "$path" "$target"
  done < <(find "$old" -mindepth 1 \( -type f -o -type l \) -print0 | sort -z)
}
for pair in ".config" ".local/state" ".local/share" ".cache"; do
  base="$home/$pair"
  [ -d "$base/noctalia" ] || continue
  echo "$pair/noctalia -> $pair/helix"
  migrate "$base/noctalia" "$base/helix"
done
if [ $dry = 1 ]; then echo "would copy $copied file(s); $kept already exist in the Helix folders"; else echo "copied $copied file(s); $kept already existed and were left alone"; fi
