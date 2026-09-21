# Helix

Helix is a niri-first Linux desktop, not a drop-in rebrand of upstream Noctalia.

## Three packages

| Package | Role |
|---|---|
| **helix-greeter** | Login (greetd session) |
| **helix-niri** | Compositor |
| **Helix Shell** (this tree) | Bars, dock, launcher, Control Center, lock screen, wallpaper, notifications, desktop widgets |

Login flow: **greetd → helix-greeter → helix-niri + Helix Shell**. Stock niri, Hyprland, and other compositors remain fallbacks.

## Names that stay `noctalia`

Keep these until a rename is an explicit release:

- Meson project and installed command: `noctalia`
- Config, state, cache: `~/.config/noctalia/`, `~/.local/state/noctalia/`, and the `NOCTALIA_*` env overrides
- IPC: `noctalia msg …`
- Desktop entry id: `dev.noctalia.Noctalia.desktop` (the **Name** is Helix Shell)
- Builtin palette id: `Noctalia`

The name in the UI, notifications, generated niri includes, template display names, docs, and package descriptions is **Helix Shell**. Template file paths (`noctalia.yaml`, `noctalia.conf`, `Noctalia.toml`) stay as compatibility names.

## Design

Liquid Glass is a hierarchy, not a skin. Chrome, leftover, modules, and widgets are documented in `.cursor/rules/helix-hig.mdc`. Control Center follows Apple HIG structure without copying private Control Center visuals.

## Lineage

This tree started from [Noctalia](https://github.com/noctalia-dev/noctalia). Compatibility names above are on purpose so existing installs keep working.
