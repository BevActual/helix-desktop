#!/usr/bin/env bash
set -euo pipefail

theme_file="${XDG_DATA_HOME:-$HOME/.local/share}/warp-terminal/themes/noctalia.yaml"
config_file="${XDG_CONFIG_HOME:-$HOME/.config}/warp-terminal/settings.toml"
custom_table="{ custom = { name = \"Helix\", path = \"${theme_file}\" } }"

if [[ ! -f "$theme_file" ]]; then
    printf 'Warning: Warp theme file not found: %s\n' "$theme_file" >&2
fi

if [[ ! -f "$config_file" ]]; then
    exit 0
fi

write_if_changed() {
    local target="$1" tmp="$2"
    if ! cmp -s "$target" "$tmp"; then
        cat "$tmp" >"$target"
    fi
    rm -f "$tmp"
}

tmp_file="$(mktemp "${config_file}.tmp.XXXXXX")"
trap 'rm -f "$tmp_file"' EXIT

awk -v custom_table="$custom_table" '
    BEGIN {
        theme_line = "theme = " custom_table
        selected_line = "selected_system_themes = { dark = " custom_table ", light = " custom_table " }"
    }

    function flush_themes_tail() {
        if (!in_themes || wrote_custom) {
            return
        }
        print theme_line
        print selected_line
        wrote_custom = 1
    }

    /^\[appearance\.themes\][[:space:]]*$/ {
        in_themes = 1
        saw_themes = 1
        print
        next
    }

    /^\[/ {
        flush_themes_tail()
        in_themes = 0
        print
        next
    }

    in_themes && /^[[:space:]]*theme[[:space:]]*=/ {
        next
    }

    in_themes && /^[[:space:]]*selected_system_themes[[:space:]]*=/ {
        next
    }

    {
        print
    }

    END {
        flush_themes_tail()
        if (!saw_themes) {
            print ""
            print "[appearance.themes]"
            print theme_line
            print selected_line
        }
    }
' "$config_file" >"$tmp_file"

trap - EXIT
write_if_changed "$config_file" "$tmp_file"
