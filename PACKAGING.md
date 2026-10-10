# Packaging Helix Shell

Notes for distribution packagers. End-user install docs live in the
[README](README.md) and [docs/user/getting-started/installation.mdx](docs/user/getting-started/installation.mdx).

## Package description

Use this exact short description for package metadata (`pkgdesc`, `Summary`,
AppStream, etc.):

> Helix Shell — a niri-first Wayland desktop shell (command: helix).

Do not substitute shorter or alternate blurbs (“lightweight Wayland bar”,
“status bar”, ...). Helix Shell is a full desktop shell (bars, dock, launcher,
notifications, lock screen, wallpaper, settings, Control Center services), not a bar-only tool.

## Packaged distributions

Upstream Helix v5 is already packaged as `helix-shell` for:

- Arch Linux
- NixOS
- Fedora
- openSUSE
- KaOS
- Gentoo
- Void Linux
- GNU Guix
- Debian (including Ubuntu)

Helix currently builds from this tree. Keep the `helix-shell` command and config paths until a rename is an explicit release.

## Identity

| | |
|---|---|
| Product | Helix Shell |
| Command | `helix-shell` |
| Homepage | https://github.com/BevActual/helix-shell |
| Docs | [`docs/user/`](docs/user/) |
| License | MIT ([LICENSE](LICENSE)); also see vendored licenses under `third_party/` for SPDX completeness |
| Version | Meson `project(... version: ...)` in [`meson.build`](meson.build) |
| Session shell (live) | `helix-shell-tauri` via `tools/switch-shell.sh install` |
| Legacy C++ session binary | Removed (Tauri only) |
| Desktop entry | `org.helix.Shell.desktop` |
| Icon | `org.helix.Shell` / `helix.svg` |

## Maintenance Policy

Releases strive to follow [semantic versioning](https://semver.org).
The latest version is the only actively maintained version.
No attempts are made to achieve overlapping lifecycles of major or minor versions.
Bug reports are required to be reproducible on the latest version.

## Architectures / libc

First-class targets:

- `x86_64` (amd64)
- `aarch64` (arm64)

Linux only. There is no intentional arch-specific code path; other little-endian
Linux arches may build if dependencies are available, but they are not
first-class supported.

**musl**: known to build (e.g. Alpine / Void musl). Not first-class tested.
`jemalloc` is glibc-only, on musl leave `-Djemalloc=disabled` (or `auto`, which
skips it).

## Compilers / linkers

C++23 is required.

| Toolchain | Minimum | Notes |
|---|---|---|
| GCC + libstdc++ | **GCC 13+** | Default on most distros |
| Clang + libstdc++ or libc++ | **Clang 16+** | libc++ needs Meson’s experimental-library flags (already wired in `meson.build`) |

Debian 12 “bookworm” needs a newer toolchain than the default `g++` (e.g.
`g++-13` with `CXX=g++-13`).

No special linker is required. The default linker from the compiler driver is
fine (**GNU ld** / gold, **lld**, **mold** via `-fuse-ld=` all work). Release
builds pass `-Wl,--gc-sections` and `-Wl,--as-needed`, which those linkers
support.

## Build

- Build system: [Meson](https://mesonbuild.com/) + Ninja. The repo’s
  [`justfile`](justfile) is convenience only; packaging can call Meson
  directly.
- Language / toolchain: see [Compilers / linkers](#compilers--linkers).
- Recommended: `meson setup build --buildtype=release`, then compile/install.
- Do **not** enable `-Dnative_optimizations=true` for distro packages (CPU-local
  codegen; not portable).
- Tests: `-Dtests=disabled` (or leave `auto`, which skips tests for release).
- `jemalloc`: recommended on glibc; Meson feature option `-Djemalloc=auto|enabled|disabled`.
  Only used on glibc builds.

Prefix/datadir are baked into the binary via `HELIX_INSTALL_PREFIX` /
`HELIX_INSTALL_DATADIR`. Install with the same prefix you configured.

### Installed layout

```text
<prefix>/bin/helix
<prefix>/share/helix/assets/...
<prefix>/share/applications/org.helix.Helix.desktop
<prefix>/share/icons/hicolor/scalable/apps/helix.svg
```

The shipped `assets/` tree is **required at runtime**. Shipping only the binary
breaks fonts, translations, templates, glyphs, and sounds. See
[CONTRIBUTING.md](CONTRIBUTING.md#runtime-assets) for lookup order (including
`HELIX_ASSETS_DIR` overrides for unusual layouts).

Not shipped (don’t look for them in the install): AppStream / metainfo XML, man
pages, systemd units.

### Shell completions

Static completion files are not shipped. Generate them from the freshly built
binary and install them in the distribution's completion directories:

```text
./build-<mode>/helix completions bash  → share/bash-completion/completions/helix
./build-<mode>/helix completions zsh   → share/zsh/site-functions/_helix
./build-<mode>/helix completions fish  → share/fish/vendor_completions.d/helix.fish
```

This is intentionally a packaging step rather than a Meson install rule:
executing the target binary during installation would break cross-compilation.

## Dependencies

The **session shell and apps** are Rust/Tauri (`shell/`, `apps/`); see
[docs/design/rust-workspace.md](docs/design/rust-workspace.md). Shared app controls
follow GTK4/libadwaita grammar ([docs/design/helix-hig.md](docs/design/helix-hig.md)).

The section below is for the **live C++ desktop authority** (`helix-service`) and
related Meson targets until Rust cutover. GLES/EGL appear where C++ helpers still
need them — not as the session UI stack.

### Build-time (required)

Canonical list is the `dependency(...)` / header checks in
[`meson.build`](meson.build). Distro package names differ; [BUILDING.md](BUILDING.md)
has copy-paste install lines for common distros.

Notes packagers hit often:

- **stb** must provide `stb/stb_image_resize2.h` (and `stb/stb_image_write.h`).
  Older packages that only ship `stb_image_resize` are not enough. Meson fails
  the configure check if `stb_image_resize2` is missing.
- Meson requires **WirePlumber 0.5** (`wireplumber-0.5` pkg-config). 0.4 is not
  enough.

### Vendored (no system package)

Shipped under `third_party/`: Wuffs, fzy, Material Color Utilities.
Each carries its own license file beside the code.

### Runtime

| Dependency | Role |
|---|---|
| PipeWire **daemon** (+ WirePlumber 0.5) | Audio, volume OSD, privacy indicators, spectrum. Libraries alone are not enough; without a running daemon those features stay off. |
| PAM (`login` service by default) | Lock screen authentication |
| Fontconfig / fonts | Text rendering (users still need usable fonts installed) |
| `upower` | Optional: battery / power devices |
| `ddcutil` | Optional: external monitor brightness |
| Secret Service provider | Optional but recommended for credential / encrypted-state persistence (GNOME Keyring, KWallet, KeePassXC, ...). `libsecret` is only the client library; without a session provider those features cannot persist secrets. |
| `sound-theme-freedesktop` | Shell sounds |

## Startup and IPC

No systemd user unit is shipped. Typical startup is compositor autostart or the
desktop entry (`helix --daemon`). Control a running instance with
`helix-panel ...` (Unix socket under `XDG_RUNTIME_DIR`).

Keep the `.desktop` daemon Exec unless you intentionally package a different
session integration. Prefer the canonical description above for `Comment=` /
AppStream summary as well.

## Session conflicts

On non-Plasma sessions Helix provides and registers:

- `org.freedesktop.Notifications`
- `org.kde.StatusNotifierWatcher` (system tray host)

Do not run it alongside another notification daemon or StatusNotifier host
(mako, dunst, swaync, waybar-as-host, ...) unless those are disabled. On Plasma,
Helix integrates with Plasma's notification / tray paths instead of claiming
the freedesktop Notifications name.

## User data paths

| Kind | Default |
|---|---|
| Config | `$XDG_CONFIG_HOME/helix` (`~/.config/helix`), e.g. `config.toml` |
| State | `$XDG_STATE_HOME/helix` (`~/.local/state/helix`), e.g. `settings.toml`, caches |
| Data | `$XDG_DATA_HOME/helix` (`~/.local/share/helix`) |
| Logs | `$XDG_CACHE_HOME/helix` (`~/.cache/helix`) |

Override bases with `HELIX_CONFIG_HOME`, `HELIX_STATE_HOME`,
`HELIX_DATA_HOME` (each still appends `/helix`).

## What Helix is not

- Not a display manager. Login is in-tree `greeter/` (`helix-greeter`).
- Not a replacement for file managers, screen casting, or drive mounting.
- The supported compositor is in-tree helix-niri (`compositor/`). Other
  compositor guides under [`docs/user/compositor-settings/`](docs/user/compositor-settings/)
  are unsupported historical notes. Product shape:
  [`docs/design/product-structure.md`](docs/design/product-structure.md).

## Versioning

Prefer packaging **tagged releases** rather than random `main` snapshots unless
you maintain a `-git` / nightly package on purpose.

## Contact

Report Helix Shell issues at https://github.com/BevActual/helix-shell/issues.

## Helix desktop service and Settings

Install the live `helix-service` binary (or the Rust `helix-desktop` binary when cut over),
`org.helix.Desktop.service` D-Bus activation file and matching systemd user unit together with
the shell. They must share the same prefix. The service is activated on demand; no globally
enabled user unit is required. See [`docs/design/cpp-to-rust.md`](docs/design/cpp-to-rust.md).

`helix-settings` is the Tauri Settings app (`apps/helix-settings`). It has its own application
entry and requires the matching desktop service for live state and edits. Helix Desktop & Dock
settings are omitted outside a Helix session. Install the Settings desktop entry only when the
application target is built.

`helix-system-monitor` is the Tauri System Monitor app. Install
`org.helix.SystemMonitor.desktop` with its binary. It needs Linux procfs for resource
metrics and runs independently of the shell, compositor and desktop service. Appearance
integration is optional and does not activate a service.
