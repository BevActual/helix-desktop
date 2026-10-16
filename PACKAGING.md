# Packaging Helix

Notes for packagers. The supported target is Fedora with helix-niri; other distributions are not a product surface.

## Package description

> Helix: a Wayland desktop built on the niri compositor, with a shell, login screen and apps that people and AI agents share.

## Source layout and build

One Git tree builds everything:

- One Cargo workspace at the root: services, session shell, greeter, apps, and the compositor crates under `compositor/`.
- npm workspace at the root for the React frontends. Each app's `build.rs` runs its build when `node_modules` exists, so a package
  build runs `npm ci` first (offline against a vendored cache, or with network access allowed for that step).
- Vendor Cargo and npm dependencies for an offline build (`cargo vendor`, an npm cache); both lock files are committed.

Build and install commands are in [BUILDING.md](BUILDING.md). The packager's usual sequence:

```sh
npm ci
cargo build --release --workspace --locked
DESTDIR="$RPM_BUILD_ROOT" tools/install.sh --no-build --require-session --prefix /usr --profile release
```

`tools/install.sh` installs everything a package ships except the compositor:

| What | Where (under the prefix) |
|---|---|
| desktop service, assistant, inference service, workers | `bin/` |
| session shell, strut, wallpaper, idle, greeter, Settings, Mobius, Software, System Monitor | `bin/` |
| `helix-greeter-apply-appearance` (Settings finds it beside itself) | `bin/` |
| `helix-panel`, `helix-askpass` | `bin/` |
| `helix-system-upgrade` (the polkit-gated helper) | `libexec/` |
| systemd user units (service, inference, assistant warm-up, strut, bar, panels, idle, wallpaper) | `lib/systemd/user/` |
| D-Bus activation files, polkit policy | `share/dbus-1/services/`, `share/polkit-1/actions/` |
| desktop entries and icons | `share/applications/`, `share/icons/hicolor/scalable/apps/` |
| theme templates, first-run wallpaper, agent policy tables | `share/helix/` |
| greeter session script and its compositor config | `bin/helix-greeter-session`, `share/helix-greeter/niri.kdl` |
| model engine libraries, when built | `lib64/helix/` |

Options: `--prefix`, `--destdir`, `--profile`, `--no-build`, `--services-only` (skip the session half and its web view libraries),
`--require-session` (fail if any session binary was not built, which is what a package wants), `--uninstall`. It honours
`CARGO_TARGET_DIR` for the binaries.

Not installed, on purpose: the compositor (`tools/install-trusted-niri.sh`), greetd's configuration (`tools/install-greeter.sh`, which
edits `/etc/greetd`), and any per-user niri configuration. Units are installed disabled; enabling is per user
(`systemctl --user enable --now helix-shell-strut helix-shell-bar helix-wallpaper helix-idle`), or with a preset file in the package.

## Identity

| | |
|---|---|
| Product | Helix |
| Homepage | https://github.com/BevActual/helix-desktop |
| License | MIT; the compositor in `compositor/` is GPL-3.0-or-later (niri fork) |
| D-Bus names | `org.helix.Desktop`, `org.helix.Inference`, `org.helix.Shell`, `org.helix.Shell.Act` |
| Config and state | `~/.config/helix`, `~/.local/state/helix`, `~/.local/share/helix` |

## Runtime dependencies

Required by the session: WebKitGTK 4.1, GTK 3, libsoup 3, gtk-layer-shell, PipeWire and WirePlumber.

Used when present (a missing one is an empty or unavailable page): NetworkManager, BlueZ, polkit, greetd, Flatpak, PackageKit, fwupd and
the systemd user manager.

Optional model engines (llama.cpp, whisper.cpp, stable-diffusion.cpp) are not packaged with Helix. They are built by hand into
`third_party/*-install` and installed with `tools/install-inference.sh`; the service runs without them using only its scripted backend.

## Files a package must not ship

`target/`, `node_modules/`, `third_party/*-install`, and `compositor/target/` are build output and can be many gigabytes.
