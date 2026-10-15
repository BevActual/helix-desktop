set positional-arguments

mode := "debug"
build-dir := "build-" + mode
prefix := "/usr/local"
cpp-std := "c++23"

default:
    @just --list

configure m=mode install_prefix=prefix:
    #!/usr/bin/env bash
    set -euo pipefail
    args=(--buildtype={{ if m == "release" { "release" } else { "debug" } }} -Dcpp_std={{cpp-std}} -Dtests=auto)
    [[ "{{m}}" == "release" ]] && args+=(-Db_lto=true)
    [[ "{{m}}" == "asan"    ]] && args+=(-Db_sanitize=address,undefined)
    if [[ -d "build-{{m}}" ]]; then
        meson setup "build-{{m}}" "${args[@]}" --prefix "{{install_prefix}}" --reconfigure
    else
        meson setup "build-{{m}}" "${args[@]}" --prefix "{{install_prefix}}"
    fi
    ln -sfn "build-{{m}}/compile_commands.json" compile_commands.json

build m=mode: (_ensure-configured m)
    # Default targets needed for `meson install` (template apply, assistant, inference, upgrade helper).
    meson compile -C build-{{m}} helix-template-apply helix-theme-palette helix-assistant helix-inference helix-system-upgrade

# Desktop service (+ optional legacy C++ shell for local experiments). Live session/apps are Tauri.
build-desktop m=mode jobs="2": (_ensure-configured m)
    mkdir -p build-{{m}}/helix-tmp
    TMPDIR="$(pwd)/build-{{m}}/helix-tmp" nice -n 10 ionice -c 3 meson compile -C build-{{m}} helix-template-apply helix-theme-palette helix-assistant helix-inference -j {{jobs}}

# Legacy GTK Settings only (needs -Ddesktop_apps=enabled). Prefer: (cargo build -p helix-settings)
build-settings m=mode jobs="2": (_ensure-configured m)
    mkdir -p build-{{m}}/helix-tmp
    TMPDIR="$(pwd)/build-{{m}}/helix-tmp" nice -n 10 ionice -c 3 meson compile -C build-{{m}} helix-settings -j {{jobs}}

_ensure-configured m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-{{m}}/build.ninja" ]]; then
        just configure {{m}}
        exit 0
    fi
    configure_output="$(meson configure "build-{{m}}")"
    current_cpp_std="$(awk '$1 == "cpp_std" { print $2; found=1 } END { if (!found) exit 1 }' <<<"$configure_output")"
    current_tests="$(awk '$1 == "tests" { print $2; found=1 } END { if (!found) exit 1 }' <<<"$configure_output")"
    args=()
    [[ "$current_cpp_std" != "{{cpp-std}}" ]] && args+=(-Dcpp_std={{cpp-std}})
    [[ "$current_tests" != "auto" ]] && args+=(-Dtests=auto)
    if (( ${#args[@]} > 0 )); then
        meson configure "build-{{m}}" "${args[@]}"
    fi

run m=mode:
    cargo run -p helix-desktop

# Build and run the unit tests, enabling their targets when auto mode omits them.
test m=mode *args: (_ensure-configured m)
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ "{{m}}" == "release" || "{{m}}" == "asan" ]]; then
        meson setup "build-{{m}}" -Dtests=enabled --reconfigure >/dev/null
    fi
    meson test -C build-{{m}} {{args}}

# Regressions for the GitHub workflow scripts. Pure Python, builds nothing.
test-workflows:
    python3 -m unittest discover -s .github/workflows/scripts -p 'test_*.py'

# Stage an installation in a temporary directory and exercise its installed CLI.
test-install m=mode: (build m)
    sh tests/install_layout_test.sh build-{{m}}

install m:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -x "build-{{m}}/helix-template-apply" ]]; then
        echo "error: build-{{m}}/helix-template-apply is missing; run 'just build {{m}}' before installing" >&2
        exit 1
    fi
    meson install --no-rebuild -C build-{{m}}
    # The Rust helix-desktop is the org.helix.Desktop owner (the unit and activation file run it).
    just install-rust-desktop {{m}}

# Build and install the Rust org.helix.Desktop service next to the meson-installed binaries.
install-rust-desktop m:
    #!/usr/bin/env bash
    set -euo pipefail
    if ! command -v cargo >/dev/null; then
        echo "error: cargo not found; helix-desktop (org.helix.Desktop) cannot be installed" >&2
        exit 1
    fi
    cargo build --release -p helix-desktop
    prefix=$(meson introspect "build-{{m}}" --buildoptions | python3 -c 'import json,sys; o={x["name"]:x["value"] for x in json.load(sys.stdin)}; print(o["prefix"] + "/" + o["bindir"])')
    install -Dm755 target/release/helix-desktop "${DESTDIR:-}${prefix}/helix-desktop"
    echo "installed ${DESTDIR:-}${prefix}/helix-desktop"

uninstall m:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-{{m}}/build.ninja" ]]; then
        echo "error: build-{{m}} is missing or was not configured with the Ninja backend; nothing to uninstall" >&2
        exit 1
    fi
    ninja -C build-{{m}} uninstall

format:
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 grep -ZlP '\s+$' | xargs -0 -r sed -i 's/[[:space:]]*$//'

_clang_tidy m=mode *args:
    #!/usr/bin/env bash
    set -euo pipefail
    meson compile -C "build-{{m}}" wayland-protocol-headers
    src_root="$(realpath src)"
    # compile_commands.json stores build-relative paths, so clang-tidy emits header
    # diagnostics as ../src/...; the header-filter must match that form (an absolute
    # ^${src_root} anchor never matches, silently dropping every header diagnostic).
    # ../src/ also excludes vendored third_party/*/src/* headers.
    run-clang-tidy -quiet -use-color -p "build-{{m}}" -j "$(nproc)" -header-filter='\.\./src/.*' {{args}} "^${src_root}/.*"

lint m=mode: (_ensure-configured m)
    just _clang_tidy {{m}} '-warnings-as-errors=*'

fix m=mode: (_ensure-configured m)
    just _clang_tidy {{m}} -fix
    just format

clean m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -L compile_commands.json && "$(readlink compile_commands.json)" == "build-{{m}}/compile_commands.json" ]]; then
        rm -f compile_commands.json
    fi
    rm -rf build-{{m}}

rebuild m=mode: (clean m) (build m)

# Tauri session shell: core lib tests, nested agent-ipc/launcher, agent policy (safe with Cursor open).
verify-shell-integrations:
    tools/verify-shell-integrations.sh
