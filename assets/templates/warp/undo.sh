#!/usr/bin/env bash
set -euo pipefail

theme_file="${XDG_DATA_HOME:-$HOME/.local/share}/warp-terminal/themes/noctalia.yaml"
config_file="${XDG_CONFIG_HOME:-$HOME/.config}/warp-terminal/settings.toml"

write_if_changed() {
    local target="$1" tmp="$2"
    if ! cmp -s "$target" "$tmp"; then
        cat "$tmp" >"$target"
    fi
    rm -f "$tmp"
}

if [[ -f "$config_file" ]]; then
    tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
    trap 'rm -f "$tmp_file"' EXIT
    awk '
        /noctalia\.yaml/ { next }
        { print }
    ' "$config_file" >"$tmp_file"
    trap - EXIT
    write_if_changed "$config_file" "$tmp_file"
fi

rm -f -- "$theme_file"
