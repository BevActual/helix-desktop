#!/usr/bin/env bash
# Builds tools/gen_settings_defaults.cpp against the helix-service objects in build-debug and
# writes the pristine-config catalog to stdout (JSON). Run from the repo root.
set -euo pipefail
B=${BUILD_DIR:-build-debug}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc=$(ninja -C "$B" -t commands helix-service | grep "src_desktop_settings_catalog.cpp.o" | head -1)
flags=${cc#c++ }
flags=${flags%% -MD*}
flags=${flags//-Ihelix-service.p/}
(cd "$B" && eval "c++ $flags -c ../tools/gen_settings_defaults.cpp -o '$out/driver.o'")
link=$(ninja -C "$B" -t commands helix-service | tail -1)
link=${link/-o helix-service /-o "$out/driver" }
link=${link/helix-service.p\/src_desktop_service_main.cpp.o/"$out/driver.o"}
(cd "$B" && eval "$link")
HOME="$out" XDG_CONFIG_HOME="$out/cfg" XDG_STATE_HOME="$out/state" HELIX_CONFIG_HOME="$out/cfg" "$out/driver" 2>/dev/null
