#!/usr/bin/env bash
set -euo pipefail

command="${1:-apply}"

resolve_config_home() {
    if [ -n "${XDG_CONFIG_HOME:-}" ]; then
        printf '%s\n' "$XDG_CONFIG_HOME"
        return
    fi

    if [ -z "${HOME:-}" ]; then
        echo "error: HOME or XDG_CONFIG_HOME must be set" >&2
        exit 1
    fi

    printf '%s/.config\n' "$HOME"
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config_dir="$(resolve_config_home)/niri"
config_file="$config_dir/config.kdl"
output_file="$config_dir/helix.kdl"
setup_file="$config_dir/helix-setup.kdl"
include_line='include "helix.kdl"'
setup_include='include "helix-setup.kdl"'

has_include() {
    local filename="$1"
    grep -Eq "^[[:space:]]*include([[:space:]].*)?\"([^\"]*/)?${filename}\"([[:space:]]|$)" "$config_file"
}

apply_include() {
    mkdir -p "$config_dir"

    if [ ! -f "$config_file" ]; then
        printf '%s\n' "$include_line" >"$config_file"
        return
    fi

    if has_include "helix.kdl"; then
        return
    else
        local grep_status=$?
        if [ "$grep_status" -ne 1 ]; then
            echo "error: failed to inspect $config_file" >&2
            exit "$grep_status"
        fi
    fi

    printf '\n%s\n' "$include_line" >>"$config_file"
}

apply_setup() {
    mkdir -p "$config_dir"
    if [ ! -f "$setup_file" ] && [ -f "$script_dir/helix-setup.kdl" ]; then
        cp "$script_dir/helix-setup.kdl" "$setup_file"
    fi
    if [ ! -f "$config_file" ]; then
        printf '%s\n' "$setup_include" >"$config_file"
        return
    fi
    if has_include "helix-setup.kdl"; then
        return
    else
        local grep_status=$?
        if [ "$grep_status" -ne 1 ]; then
            echo "error: failed to inspect $config_file" >&2
            exit "$grep_status"
        fi
    fi
    printf '\n%s\n' "$setup_include" >>"$config_file"
}

case "$command" in
    output)
        printf '%s\n' "$output_file"
        ;;
    apply)
        apply_include
        apply_setup
        ;;
    *)
        echo "Usage: $0 {output|apply}" >&2
        exit 1
        ;;
esac
