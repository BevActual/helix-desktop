# Building Helix from source

Helix is a Cargo workspace (Rust) with npm-built frontends, plus the niri fork in `compositor/`, whose crates are members of the same workspace. Fedora with
helix-niri is the one supported target. Layout: [docs/design/product-structure.md](docs/design/product-structure.md).

## What builds what

| Part | Built by | Needs |
|---|---|---|
| Desktop service, assistant, inference service, upgrade helper | `cargo build -p helix-desktop -p helix-assistant -p helix-inference -p helix-system-upgrade` | Rust, a C compiler, `clang` and PipeWire headers |
| Session shell, greeter, Settings, Mobius, Software, System Monitor | `cargo build --workspace` (each app's `build.rs` runs its npm build when `node_modules` exists) | the above, plus WebKitGTK, GTK 3, libsoup 3, gtk-layer-shell; Node and npm for the frontends |
| Compositor | `cargo build --release -p niri` (or `just build-compositor release`) | the above, plus libinput, libseat, gbm, udev, xkbcommon, pango and libdisplay-info development packages |
| Model engines (optional) | by hand into `third_party/*-install`; see [inference-capabilities.md](docs/design/inference-capabilities.md) | CMake, a C++ compiler |
| Install layout | `tools/install.sh` | nothing beyond the build |

There is no Meson, CMake or autotools step for Helix itself. The only C and C++ compiled is the engine shims under `services/*/native/`,
and only when an engine is present.

## Dependencies (Fedora)

```sh
sudo dnf install rust cargo gcc gcc-c++ clang pkgconf-pkg-config just \
  webkit2gtk4.1-devel gtk3-devel libsoup3-devel gtk-layer-shell-devel librsvg2-devel \
  pipewire-devel dbus-devel libsecret-devel \
  nodejs npm
```

These are the packages present on the reference build machine. Without `nodejs` and `npm` the Rust build still succeeds and the apps fall
back to their hand-written pages (cargo prints a warning); `HELIX_SKIP_FRONTEND=1` does the same on purpose.

Runtime services Helix talks to, when present: PipeWire and WirePlumber (`wpctl`), NetworkManager, BlueZ, polkit, greetd, Flatpak,
PackageKit and fwupd. A missing one shows as an empty or "not available" page, not an error.

Tests also want `python3-gobject`, `dbus-daemon` and, for the audio meter, `pipewire` with its command-line tools.

`rust-toolchain.toml` pins the stable channel with `rustfmt` and `clippy` for rustup users, so all four tools come from one install.
If `cargo` and `rustc` come from different places (for example Fedora's `rustc` and a rustup `clippy-driver` on one `PATH`), builds
fail with "found crate compiled by an incompatible version of rustc". Put one toolchain first on `PATH`.

## Build

```sh
just build               # whole workspace, `fast` profile (quick link, used by tests and iteration)
just build release       # release profile (LTO, what gets installed)
just build-services      # only the services, no web view libraries needed
```

Profiles are defined in the root `Cargo.toml`: `fast` (opt-level 1, no LTO, incremental) and `release`. Compiler output goes to `target/`.
`just` is a convenience; every recipe is a plain `cargo` command or a script in `tools/`.

## Test

```sh
just test                   # cargo test --workspace, then the integration tests
just test-integration       # only the integration tests (private D-Bus session, built binaries)
tools/test-integration.sh layering assistant_integration    # chosen ones by name
just check                  # lints, clippy -D warnings, tests, integration tests, nested UI suite (tools/check-all.sh)
```

A test whose requirements are missing is reported as skipped. The nested UI suite (`tools/nested/ui-suite.sh`) runs the real apps in a
headless nested compositor and drives them with a virtual pointer.

## Install

For yourself, the session shell and apps (user level, nothing needs root):

```sh
tools/switch-shell.sh install
```

System level (services, the session shell and apps, greeter, systemd user units, D-Bus activation, polkit policy, desktop entries,
icons, theme templates and agent policy tables):

```sh
just install                          # /usr/local, release profile
just install /usr                     # another prefix
DESTDIR=/tmp/stage just install /usr  # stage a tree (packagers)
just uninstall                        # remove what install put under the prefix
```

`tools/install.sh --help` lists the options (`--services-only` skips the session half and the web view libraries it needs). It does
not touch greetd's configuration (`tools/install-greeter.sh` writes `/etc/greetd`), the compositor (`tools/install-trusted-niri.sh`)
or the per-user niri configuration; the model runtime has its own `tools/install-inference.sh`.
