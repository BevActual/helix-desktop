# Helix Shell

Helix Shell is a native Wayland desktop shell. It gives you a polished, configurable Linux desktop without stitching
together a separate bar, launcher, notification daemon, lock screen, wallpaper tool, settings UI, and a pile of
service panels.

It provides the shell layer around your compositor: bars, widgets, dock, launcher, control center, notifications,
wallpaper, lock screen, session actions, clipboard history, OSDs, tray integration, and desktop widgets. Control Center
also owns first-party services (packages, Tailscale, LocalSend, Phone, Syncthing, Firewall, Printers, Cast, Privacy, and similar
tools). The project is built directly on Wayland and OpenGL ES with no Qt or GTK dependency, so the UI, rendering,
configuration, and IPC model are one cohesive shell.

This tree started from [Noctalia](https://github.com/noctalia-dev/noctalia). The binary, assets, and config paths are
still `noctalia`. The product name in the UI is Helix Shell.

<p><br/></p>

<p align="center">
  <img src="https://assets.noctalia.dev/noctalia-logo.svg?v=2" alt="Helix Shell" style="width: 192px" />
</p>

<p><br/></p>

<p align="center">
  <a href="https://github.com/noctalia-dev/noctalia/commits">
    <img src="https://img.shields.io/github/last-commit/noctalia-dev/noctalia?style=for-the-badge&labelColor=FFF59B&color=FFF59B&logo=git&logoColor=070722&label=commit" alt="Last commit" />
  </a>
  <a href="https://docs.noctalia.dev">
    <img src="https://img.shields.io/badge/docs-FFF59B?style=for-the-badge&logo=gitbook&logoColor=070722&labelColor=FFF59B" alt="Documentation" />
  </a>
</p>

## Why Helix Shell?

Most Wayland setups leave the desktop shell to a stack of small tools: one bar, another launcher, another notification
daemon, a lock screen, a wallpaper daemon, scripts for session actions, and separate config formats for each piece. That
can be flexible, but it also makes a complete desktop feel fragile and hard to keep visually consistent.

Helix solves that by owning the common desktop surfaces and the service layer in Control Center, while still fitting into
compositor-driven Wayland workflows. Tabs you do not use can be hidden. A service whose daemon is missing stays in the
sidebar with a start or empty card, the same pattern as Tailscale and Phone.

## What It Includes

- Multi-monitor bars with configurable widgets, taskbar, workspaces, system tray, media, network, battery, brightness,
  weather, clipboard, and custom script-backed widgets.
- Dock, launcher, control center, notification toasts/history, wallpaper picker, OSD overlays, lock screen, session
  panel, and desktop widgets.
- Control Center services: packages, Tailscale, LocalSend, Phone (KDE Connect), Syncthing, Firewall, Printers, Cast,
  Privacy, screen time, AI, capture, and clipboard history.
- TOML configuration with hot reload, GUI-managed overrides, theme/palette support, template application, and IPC for
  runtime control.
- Direct Wayland integration for layer-shell, session lock, idle behavior, clipboard, foreign toplevels, workspaces,
  fractional scaling, and compositor-specific workspace backends where needed.

## Wayland Compositor Support

Helix Shell supports Wayland compositors that provide the layer-shell protocols it needs for shell surfaces. Workspace
integration works through compositor-native backends where needed, or through `ext-workspace-v1` on compositors that
implement it.

Current compositor integrations include Niri, Hyprland, Sway, Scroll, Mango, Labwc, Triad, dwl, and other compatible
Wayland compositors. Other compositors may run Helix but can have reduced workspace, window, output, or session-action
integration depending on the protocols and IPC they expose.

## Scope

Helix Shell is the desktop shell around your Wayland compositor. It owns bars, panels, launcher, notifications, dock,
lock screen, idle behavior, OSDs, theming, wallpapers, desktop widgets, multi-monitor shell surfaces, and the Control
Center service layer.

The compositor still owns window management and tiling. Helix does not replace niri, Hyprland, or Sway as a window
manager.

First-party Control Center services are in scope when they have live state, a primary action, and a native backend.
Hide any tab except Home with `hidden_tabs` or **Control Center → Control Center**. That includes sharing, device,
printer, drive, and casting pages when they earn a tab. A missing backend is an empty or start card, not a reason to
leave the feature out of Helix.

Display and login greeter support can live in a separate greeter project. Helix may integrate with that when useful,
but it does not replace the greeter.

The plugin system is still available for user-installed extras: additional bar widgets, launcher providers, desktop
widgets, panels, shortcuts, and hardware-specific controls. Prefer a first-party tab when the service matches how
Helix already works (a native C++ service, a live snapshot, and a hideable sidebar entry).

## Build from source

Source dependencies, distro-specific package commands, build modes, and install layouts live in
[BUILDING.md](BUILDING.md).

## Configuration

A ready-to-use starting config with all defaults is at [example.toml](example.toml). The full configuration reference
lives in the [documentation site](https://docs.noctalia.dev/noctalia/). The source MDX files are in
[`docs/user/`](docs/user/); sync them to a local docs checkout with `tools/sync-docs.sh`.

## Contributing

Developer notes, architecture overview, code style, project layout, and debugging commands live in
[CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT License. See [LICENSE](LICENSE) for details.

## Packaging

Distro packaging notes (description, deps, install layout, Meson options) live in
[PACKAGING.md](PACKAGING.md).
