# Helix Desktop

**Helix Desktop** is the monorepo for [Helix](docs/design/helix-identity.md): a niri-first Linux desktop — login (`greeter/`), compositor (`compositor/`), session shell, apps, and desktop services in one product tree.

**Helix Shell** is the session chrome only — bars, dock, launcher, Control Center, lock screen, wallpaper, notifications, and desktop widgets (live build: `shell/helix-shell-tauri`).

Product structure and runtime roles: [docs/design/product-structure.md](docs/design/product-structure.md). Design index: [docs/design/README.md](docs/design/README.md).

The **live session shell is the Tauri/Rust build** (`shell/helix-shell-tauri`, installed with `tools/switch-shell.sh install`). The C++ session UI is removed; see [docs/design/session-shell.md](docs/design/session-shell.md). Desktop authority is **`helix-desktop`** (`services/helix-desktop`, bus name `org.helix.Desktop`, unit `helix-service.service`); contracts under `protocols/` are the shared boundary.

The compositor lives in-tree at [`compositor/`](compositor/) (helix-niri fork; crates in the same Cargo workspace). Login is the Tauri greeter at [`greeter/`](greeter/) (`helix-greeter`). **Fedora + helix-niri are the sole shell target**; other compositors are not a supported product surface.

It gives you a polished, configurable desktop without stitching together a separate bar, launcher, notification daemon, lock screen, wallpaper tool, settings UI, and a pile of service panels. Control Center also owns first-party services (packages, Tailscale, Firewall, Printers, Privacy, and similar tools).

Helix is also being built as an **agentic OS**: the Agent Surface exposes the session to approved agents (observe and semantic act), **Mobius** is the local AI hub, and every desktop action by an agent goes through grants and a confirmation dialog. See [docs/design/agentic-os.md](docs/design/agentic-os.md) and [docs/design/inference-fabric.md](docs/design/inference-fabric.md).

The product name, install paths, and IPC are **Helix** (see `docs/design/helix-identity.md`).

<p><br/></p>

<p align="center">
  <img src="assets/helix.svg" alt="Helix" style="width: 192px" />
</p>

<p><br/></p>

## Why Helix Shell?

Most Wayland setups leave the desktop shell to a stack of small tools: one bar, another launcher, another notification
daemon, a lock screen, a wallpaper daemon, scripts for session actions, and separate config formats for each piece. That
can be flexible, but it also makes a complete desktop feel fragile and hard to keep visually consistent.

Helix solves that by owning the common desktop surfaces and the service layer in Control Center, while still fitting into
compositor-driven Wayland workflows. Tabs you do not use can be hidden. A service whose daemon is missing stays in the
sidebar with a start or empty card, the same pattern as Tailscale.

## What It Includes

- Multi-monitor bars with configurable widgets, taskbar, workspaces, system tray, media, network, battery, brightness,
  weather, clipboard, and custom script-backed widgets.
- Dock, launcher, control center, notification toasts/history, wallpaper picker, OSD overlays, lock screen, session
  panel, and desktop widgets.
- Control Center services: packages, Tailscale, Firewall, Printers, Privacy, screen time, AI, capture, and clipboard
  history.
- TOML configuration with hot reload, GUI-managed overrides, theme/palette support, template application, and IPC for
  runtime control.
- Direct Wayland integration for layer-shell, session lock, idle behavior, clipboard, foreign toplevels, workspaces,
  fractional scaling, and compositor-specific workspace backends where needed.

## Wayland compositor

Helix Shell targets the in-tree compositor at [`compositor/`](compositor/) (helix-niri). That fork owns window
management and tiling; the shell owns layer-shell chrome and desktop services. **Fedora + helix-niri is the sole
supported product surface** — see [docs/design/product-structure.md](docs/design/product-structure.md).

## Scope

Helix Shell is the desktop shell around helix-niri. It owns bars, panels, launcher, notifications, dock,
lock screen, idle behavior, OSDs, theming, wallpapers, desktop widgets, multi-monitor shell surfaces, and the Control
Center service layer.

First-party Control Center services are in scope when they have live state, a primary action, and a native backend.
Hide any tab except Home with `hidden_tabs` or **Control Center → Control Center**. A missing backend is an empty or
start card, not a reason to leave the feature out of Helix.

Login lives in `greeter/` (`helix-greeter`). Helix Shell syncs appearance to that greeter; it does not replace greetd.

The Luau plugin system was removed (2026-10-05). Extra bar widgets, launcher providers, panels and shortcuts will
return as first-party features or as a new extension mechanism.

## Build from source

**Session shell and apps (live path):** see [docs/design/rust-workspace.md](docs/design/rust-workspace.md) and `tools/switch-shell.sh install`.

**Everything else:** Cargo builds the whole workspace (`just build`), `just test` runs the tests, and `just install` installs the services.
Dependencies, profiles and install options are in [BUILDING.md](BUILDING.md). The Rust `helix-desktop` owns `org.helix.Desktop`.

## Configuration

A ready-to-use starting config with all defaults is at [example.toml](example.toml). The configuration reference lives
in [`docs/user/`](docs/user/). Config files still use `~/.config/helix/`. Sync docs to a local docs checkout with
`tools/sync-docs.sh`.

## Contributing

Developer notes, architecture overview, code style, project layout, and debugging commands live in
[CONTRIBUTING.md](CONTRIBUTING.md). Product contracts with helix-niri and helix-greeter live in [HELIX.md](HELIX.md).

## License

MIT License. See [LICENSE](LICENSE) for details.

## Packaging

Packaging notes (description, runtime dependencies, install layout) live in [PACKAGING.md](PACKAGING.md).

### Native desktop applications

Helix Settings, Software, and System Monitor are Tauri apps under `apps/`.
Settings talks to `helix-desktop`; System Monitor reads local Linux resources without the shell.
See `docs/design/desktop-service.md` and `docs/design/rust-workspace.md`.
