# Helix

Helix is a niri-first Linux desktop: login, compositor, and shell as one product tree.

**How it should be structured and run:** [`docs/design/product-structure.md`](docs/design/product-structure.md).  
**Design index:** [`docs/design/README.md`](docs/design/README.md).

## Packages in this repository

| Path | Role |
|---|---|
| **greeter/** | Login greeter (Tauri; greetd) |
| **compositor/** | Helix compositor (niri fork; own Cargo workspace) |
| **shell/**, **apps/**, **services/**, **core/** | Helix Shell (Tauri + desktop authority + helpers) |

Login flow: **greetd → helix-greeter → helix-niri + Helix Shell**. Fedora and helix-niri are the sole shell target.

## Session shell (live)

```sh
tools/switch-shell.sh install
```

Compositor / greeter (from this tree):

```sh
cd compositor && cargo build --release
cargo build --profile fast -p helix-greeter
```

Standalone apps under `apps/` (shared UI in `core/shared-ui/`). Desktop authority: C++ `helix-service` today, Rust scaffold `services/helix-desktop` (`docs/design/cpp-to-rust.md`). Contracts: `protocols/`. Product names: `docs/design/helix-identity.md`. Tree layout: `docs/design/session-shell.md`. Compositor: `compositor/HELIX.md`. Greeter: `greeter/HELIX.md`.

## Repository map

| Path | Role |
|---|---|
| `greeter/` | Tauri login greeter (`helix-greeter`) |
| `compositor/` | niri fork (excluded from root Cargo workspace) |
| `core/` | Pure core, desktop API client, shared-ui |
| `shell/` | Live session shell (Tauri) |
| `apps/` | Settings, Software, Monitor, Mobius (`helix-mobius`) |
| `services/` | `helix-desktop` (Rust Desktop scaffold), wallpaper, idle |
| `dev/` | Nested probes only |
| `protocols/` | D-Bus / IPC contracts |
| `tools/` | Dev install, nested tests, Helix MCP server (`helix-agent`), helpers |
| `src/desktop/` | `helix-service` (keep) |
| `src/assistant/`, `src/inference/` | Assistant + model runtime (target: `services/helix-*`) |
| `src/` (platform) | config, theme, dbus, wayland, … (migrating with domains) |
| `assets/` | Branding, templates, translations |
| `docs/` | Design + user docs (`design/archive/` for dumps) |


## Checks

```sh
tools/check-all.sh              # clippy -D warnings, cargo test, meson test, then the nested UI suite
tools/check-all.sh --no-nested  # skip the nested compositor runs
tools/nested/ui-suite.sh dock mobius   # just some of the nested tests
tools/migrate-state.sh --dry-run       # what would be copied from the old noctalia folders (additive, never overwrites)
```

Nested tests run a private compositor and bus. They share the real user's systemd and login manager, so code that would touch those
(start a unit, suspend, power off) checks `helix_shell_core::nested_test_session()` and does nothing there.

## Design

GTK4/libadwaita defines shared controls and application styling; glass remains floating shell chrome. Full language: `docs/design/helix-hig.md`.

Helix is an **agentic OS** (Mobius hub + bar Assistant + MCP layer). Fabric: `docs/design/inference-fabric.md`. Surface: `docs/design/agentic-os.md`.
