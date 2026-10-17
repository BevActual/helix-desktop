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

Build and install commands are in [BUILDING.md](BUILDING.md). The sequence a non-RPM packager runs:

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

## RPM

`packaging/helix.spec` builds three packages from this tree. Fedora on x86_64 is the only target that has been built and checked.

| Package | Holds | License | Why separate |
|---|---|---|---|
| `helix` | the services, the session shell and the apps, the systemd user units and a user preset, D-Bus activation files, polkit policy, desktop entries, icons, theme templates, agent policy tables | MIT, and the licenses of what it links | the product |
| `helix-niri` | the compositor as `/usr/bin/niri`, `niri-session`, the Wayland session entry, portal config, units, shell completions, a system `/etc/niri/config.kdl` | GPL-3.0-or-later, and what it links | a different license, and a person may want only the compositor |
| `helix-greeter` | the login screen, its session script, its private compositor config, a greetd configuration to copy | MIT, and what it links | it needs greetd and changes how logging in works, so it is opt-in |

Build, from a clean checkout (a package must not be built from a tree with a model engine in `third_party/`; rpmbuild rejects the build path it
leaves in the binaries):

```sh
tools/packaging/make-sources.sh              # rpm-sources/: the committed tree, vendored crates, node_modules
rpmbuild -bb --define "_sourcedir $PWD/rpm-sources" packaging/helix.spec
```

`make-sources.sh` needs network once; the build does not (`--locked --offline`, the crates vendored with the cargo config that points at them, `node_modules` unpacked so each
app's `build.rs` builds its web view). Make the two dependency archives again whenever `Cargo.lock` or `package-lock.json` changes.

Check the spec without a release build: `tools/packaging/rpm-test.sh` builds the packages from the binaries already in `target/fast` (build them first, as its header says) and
checks what came out: every installed file is packaged, the compositor replaces `niri`, nothing touches `/etc/greetd`, every unit the preset enables is shipped, the license lines are current.

### Decisions

* **The compositor installs `/usr/bin/niri`, conflicts with the distribution's `niri` and provides it.** The binary keeps its name (`compositor/HELIX.md`), the greeter and
  `niri-session` run `/usr/bin/niri`, and the shell finds the fork by the `helix` in its version string. Renaming it would need every launcher changed for no gain; the cost
  is that installing it removes stock niri, which is what a person who wants Helix's compositor expects.
* **`/etc/niri/config.kdl` is the default config plus the `trusted-surface` block naming `/usr/bin/helix-shell-tauri`.** Without that block the compositor cannot protect the
  approval dialog, and the shell then refuses confirm-tier actions (`docs/design/trusted-surface.md`). A person with their own `~/.config/niri/config.kdl` includes
  `/usr/share/helix/niri/helix-trusted-surface.kdl` in it. It is `%config(noreplace)`.
* **Units are enabled by a preset, not by the person.** `90-helix.preset` enables the strut, bar, wallpaper and idle units; the package's post-install script applies it
  (`systemd-update-helper install-user-units`). The panels unit is wanted by the bar; the desktop and inference services start by D-Bus activation.
* **greetd is never written.** The greeter package ships `/usr/share/helix-greeter/greetd-config.toml` to copy; replacing `/etc/greetd/config.toml` is the administrator's act, as
  `tools/install-greeter.sh` makes it for a source install (with a backup).
* **The model engines and their workers are not packaged** (`helix-transcribe`, `helix-image` are removed from the install). They are built by hand into `third_party/`; the
  inference service runs without them.
* **Bundled dependencies.** The crates and node modules are vendored, which Fedora's own repositories do not accept; this spec is for a COPR or an own repository. The
  `License:` lines are generated from Cargo metadata and `package-lock.json` (`tools/packaging/licenses.py --write`, checked by `rpm-test.sh`). `bundled(crate(...))`
  provides are not generated.
* **No debuginfo package** (`debug_package` is nil): Rust debuginfo for this tree is very large. The workspace's release profile decides code generation; the build keeps
  only the link hardening from the distribution's flags.

### Not done

Signing, a COPR project and its automatic builds, a CI job that runs `rpm-test.sh`, aarch64, SELinux policy for the greeter's runtime directory, `bundled()` provides.

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
