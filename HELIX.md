# Helix

Helix is a niri-first Linux desktop, not a drop-in rebrand of upstream Noctalia.

## Three packages

| Package | Role |
|---|---|
| **helix-greeter** | Login (greetd session) |
| **helix-niri** | Compositor |
| **Helix Shell** (this tree) | Bars, dock, launcher, Control Center, lock screen, wallpaper, notifications, desktop widgets |

Login flow: **greetd → helix-greeter → helix-niri + Helix Shell**. Fedora and helix-niri are the sole shell target. Other compositor backends are retained only until the migration cutover.

## Names that stay `noctalia`

Keep these until a rename is an explicit release:

- Meson project and installed command: `noctalia`
- Config, state, cache: `~/.config/noctalia/`, `~/.local/state/noctalia/`, and the `NOCTALIA_*` env overrides
- IPC: `noctalia msg …`
- Desktop entry id: `dev.noctalia.Noctalia.desktop` (the **Name** is Helix Shell)
- Builtin palette id: `Noctalia`

The name in the UI, notifications, generated niri includes, template display names, docs, and package descriptions is **Helix Shell**. Template file paths (`noctalia.yaml`, `noctalia.conf`, `Noctalia.toml`) stay as compatibility names.

## Design

GTK4/libadwaita defines shared controls and application styling; glass remains floating shell chrome. The full design language (tiers, color, shape, type, motion, menus, accessibility, and known gaps) is in `docs/design/helix-hig.md`. Control Center follows Apple HIG structure without copying private Control Center visuals.

Helix is evolving toward an **agentic OS** (Agent Center hub + Assistant presence). The session
**Agent Surface** contract lives in `docs/design/agentic-os.md`; v1 is IPC + MCP adapters, not the
assistant UI yet.

## Lineage

This tree started from [Noctalia](https://github.com/noctalia-dev/noctalia). Compatibility names above are on purpose so existing installs keep working.

## Independent services and applications

`helix-service` owns configuration persistence, appearance exports and the migrated audio/network/
Bluetooth backends. `helix-settings` is an adaptive GTK4/libadwaita application that can run without
Helix Shell or helix-niri. The shell retains rendering, session surfaces and audio playback/observation.
The migration inventory in `docs/design/desktop-migration.json` tracks feature parity.
The user explicitly retired all legacy Settings editors on 2026-10-05 before
replacement parity; unavailable advanced controls must be rebuilt in GTK Settings.
`docs/design/desktop-transition-handoff.md` records the current
implementation state, next slices and verification criteria for contributors.
Settings commands now launch GTK Settings, and Control Center defaults to quick Home.
The full desktop migration remains incomplete.
