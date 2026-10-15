# Packaging Helix

Notes for packagers. The supported target is Fedora with helix-niri; other distributions are not a product surface.

## Package description

> Helix: a Wayland desktop built on the niri compositor, with a shell, login screen and apps that people and AI agents share.

## Source layout and build

One Git tree builds everything:

- Cargo workspace at the root (services, session shell, greeter, apps); `compositor/` is a second, separate workspace.
- npm workspace at the root for the React frontends. Each app's `build.rs` runs its build when `node_modules` exists, so a package
  build runs `npm ci` first (offline against a vendored cache, or with network access allowed for that step).
- Vendor Cargo and npm dependencies for an offline build (`cargo vendor`, an npm cache); both lock files are committed.

Build and install commands are in [BUILDING.md](BUILDING.md). The packager's usual sequence:

```sh
npm ci
cargo build --release --workspace --locked
DESTDIR="$RPM_BUILD_ROOT" tools/install.sh --no-build --prefix /usr --profile release
```

`tools/install.sh` installs the services and their units, D-Bus activation files, polkit policy, icon and agent policy tables. It does
not yet install the session shell, greeter and apps; a package must also install those binaries (`helix-shell-tauri`, `helix-greeter`,
`helix-settings`, `helix-mobius`, `helix-software`, `helix-system-monitor`), the `assets/*.desktop` entries and the `tools/systemd/*.in`
units (their `@SHELL_BIN@`, `@STRUT_BIN@`, `@IDLE_BIN@`, `@WALLPAPER_BIN@` and `@POLKIT_AGENT@` placeholders are filled in by
`tools/switch-shell.sh`). Moving that into `tools/install.sh` is the next step toward an RPM spec.

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
