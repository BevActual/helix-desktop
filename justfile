set positional-arguments

mode := "debug"
build-dir := "build-" + mode
prefix := "/usr/local"

default:
    @just --list

configure m=mode install_prefix=prefix:
    #!/usr/bin/env bash
    set -euo pipefail
    args=(--buildtype={{ if m == "release" { "release" } else { "debug" } }} -Dtests=auto)
    [[ "{{m}}" == "release" ]] && args+=(-Db_lto=true)
    [[ "{{m}}" == "asan"    ]] && args+=(-Db_sanitize=address,undefined)
    if [[ -d "build-{{m}}" ]]; then
        meson setup "build-{{m}}" "${args[@]}" --prefix "{{install_prefix}}" --reconfigure
    else
        meson setup "build-{{m}}" "${args[@]}" --prefix "{{install_prefix}}"
    fi
    ln -sfn "build-{{m}}/compile_commands.json" compile_commands.json

# Cargo owns session services, config export, and inference workers.
build m=mode: (_ensure-configured m)
    cargo build -p helix-shell-core --bin helix-rust-config-export

# Inference fabric, speech worker, and image worker (engines optional).
build-inference profile="fast":
    cargo build --profile {{profile}} -p helix-inference --bin helix-inference --bin helix-transcribe
    cargo build --profile {{profile}} -p helix-image

# Rust template-engine driver for `core/helix-theme/tools/regen_golden_template.sh`.
build-theme-golden:
    cargo build -p helix-theme --bin gen-golden-template

# Desktop service. Live session and apps are Tauri.
build-desktop m=mode jobs="2": (_ensure-configured m)
    cargo build -p helix-shell-core --bin helix-rust-config-export

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
    current_tests="$(awk '$1 == "tests" { print $2; found=1 } END { if (!found) exit 1 }' <<<"$configure_output")"
    args=()
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
    meson install --no-rebuild -C build-{{m}}
    # The Rust helix-desktop is the org.helix.Desktop owner (the unit and activation file run it).
    just install-rust-desktop {{m}}

# Build and install Rust session services (Desktop, assistant, inference + workers, upgrade helper).
install-rust-desktop m:
    #!/usr/bin/env bash
    set -euo pipefail
    if ! command -v cargo >/dev/null; then
        echo "error: cargo not found; helix-desktop (org.helix.Desktop) cannot be installed" >&2
        exit 1
    fi
    cargo build --release -p helix-desktop -p helix-assistant -p helix-inference -p helix-system-upgrade
    prefix=$(meson introspect "build-{{m}}" --buildoptions | python3 -c 'import json,sys; o={x["name"]:x["value"] for x in json.load(sys.stdin)}; print(o["prefix"] + "/" + o["bindir"])')
    install -Dm755 target/release/helix-desktop "${DESTDIR:-}${prefix}/helix-desktop"
    install -Dm755 target/release/helix-assistant "${DESTDIR:-}${prefix}/helix-assistant"
    install -Dm755 target/release/helix-inference "${DESTDIR:-}${prefix}/helix-inference"
    if [[ -x target/release/helix-transcribe ]]; then
        install -Dm755 target/release/helix-transcribe "${DESTDIR:-}${prefix}/helix-transcribe"
    else
        echo "note: helix-transcribe not built (needs third_party/whisper-install); skipping" >&2
    fi
    libexec=$(meson introspect "build-{{m}}" --buildoptions | python3 -c 'import json,sys; o={x["name"]:x["value"] for x in json.load(sys.stdin)}; print(o["prefix"] + "/" + o["libexecdir"])')
    install -Dm755 target/release/helix-system-upgrade "${DESTDIR:-}${libexec}/helix-system-upgrade"
    echo "installed ${DESTDIR:-}${prefix}/{helix-desktop,helix-assistant,helix-inference[,helix-transcribe]} and ${DESTDIR:-}${libexec}/helix-system-upgrade"

uninstall m:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-{{m}}/build.ninja" ]]; then
        echo "error: build-{{m}} is missing or was not configured with the Ninja backend; nothing to uninstall" >&2
        exit 1
    fi
    ninja -C build-{{m}} uninstall

format:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -d cpp ]]; then
        echo "no platform C++ sources"
        exit 0
    fi
    find cpp tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 -r clang-format -i
    find cpp tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 -r grep -ZlP '\s+$' | xargs -0 -r sed -i 's/[[:space:]]*$//'

_clang_tidy m=mode *args:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -d cpp ]]; then
        echo "no platform C++ sources"
        exit 0
    fi
    cpp_root="$(realpath cpp)"
    run-clang-tidy -quiet -use-color -p "build-{{m}}" -j "$(nproc)" -header-filter='\.\./cpp/.*' {{args}} "^${cpp_root}/.*"

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
