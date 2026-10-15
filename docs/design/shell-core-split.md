# Splitting helix-shell-core: sizing

`core/helix-shell-core` holds 83 files and 36k lines: the settings catalog and config export, device and app view models, agent policy
and the shell's surface models. Every service and app depends on all of it, so an edit anywhere rebuilds everything that links it.
This page sizes splitting it into focused crates. Measured and counted on 2026-10-15; nothing has been moved yet.

## What the graph says

| Directory | Files | Lines |
|---|---|---|
| `desktop/` | 36 | 19,659 |
| `shell/` | 19 | 7,170 |
| `apps/` | 9 | 4,691 |
| `agent/` | 16 | 4,505 |

The modules are less tangled than the crate size suggests. Of 28 edges between the four directories, **25 point at one 195 line file**,
`agent/display.rs` (`sanitize_for_display`, `friendly_client_name`). The rest are three real seams, listed below. There are no cycles
between the crates proposed here once those are cut.

## Proposed crates

| Crate | From | Files | Lines | Used by |
|---|---|---|---|---|
| `helix-display` | `agent/display` | 1 | 195 | everything below |
| `helix-agent` | rest of `agent/` | 15 | 4,310 | desktop service, assistant, shell, Settings |
| `helix-config` | `desktop/` catalog, settings, ordered lists, prefs, merge and migrate; `apps/inference_routing` | 17 | 10,195 | desktop service, Settings, Mobius |
| `helix-config-export` | `desktop/config_export*` | 8 | 5,875 | desktop service, `helix-rust-config-export` |
| `helix-models` | audio, bluetooth, network, brightness, power, pairing, quick, wallhaven | 8 | 3,127 | shell, Settings |
| `helix-compositor` | compositor config and IPC, capture, shortcuts, `shell/niri`, `shell/agent_desk` | 6 | 1,505 | shell, Settings, strut |
| `helix-shell-model` | the rest of `shell/` (bar, dock, panels, tray, island, launcher, notifications) | 17 | 6,564 | shell, strut |
| `helix-app-models` | `apps/` (chat, personas, assistant session, software, monitor, markdown) | 8 | 4,254 | Mobius, Software, Monitor, shell |

Dependency order, with no back-edges: `helix-display` ← everything; `helix-compositor` ← `helix-shell-model` ← `helix-agent`;
`helix-config` ← `helix-config-export`, `helix-app-models`.

## What each consumer actually uses

| Crate | Uses (reference count) |
|---|---|
| `helix-desktop` | config 18, agent 13, config-export 1 |
| `helix-assistant` | agent 4 |
| `helix-mobius` | app models 25, prefs 7 |
| `helix-software`, `helix-system-monitor` | app models 4 and 2 |
| `helix-settings` | prefs and compositor 20, models 7, agent 6, config 2 |
| `helix-shell-tauri` | shell 68, models 15, prefs and compositor 13, agent 13, app models 6 |

Each binary needs one to three of the proposed crates. That is where the build-time win comes from: Software and System Monitor would
stop rebuilding when the settings catalog changes, and the desktop service would stop rebuilding when chat changes.

Measured today, in `cargo check` mode (builds and tests cost more): after editing `apps/chat.rs`, which only Mobius uses,
`helix-desktop` re-checks in 13.8 s, `helix-assistant` in 5.2 s and `helix-software` in 6.0 s.

## The seams that need code changes

| Edge | Fix |
|---|---|
| `agent/agent_msg` uses `shell_surface::PANELS` | Move the panel id list into `helix-agent`; `shell_surface` re-exports it |
| `agent/agent_status` uses `dock::WindowFact` | Move `WindowFact` to `helix-compositor` (it is a compositor window fact), `dock` re-exports it |
| `shell/agent_desk` and `desktop/compositor_config` use each other | Both go into `helix-compositor`; the cycle disappears |
| `desktop/settings` uses `apps/inference_routing` | `inference_routing` moves into `helix-config`; `helix-app-models` then depends on it (one edge) |
| `desktop/settings` uses `config_merge` and `config_settings_migrate` | These belong with the catalog, not the export |

## Hazards

- **Compile-time paths.** Five `include_str!` calls reach `../../../../protocols/...` or `tests/golden/...`, and seven uses of
  `CARGO_MANIFEST_DIR` assume the crate root (`agent/risk.rs`, `agent_msg.rs`, `tool_schema.rs`, `config_export_defaults.rs`,
  `config_export_serialize.rs`, `ordered_catalog.rs`, `config_export*.rs`). Each changes with the directory depth of its new crate.
- **TypeScript bindings.** 27 `ts-rs` exports in 12 files will land in four crates. `tools/gen-bindings.sh` runs one crate; it must loop,
  and the generated `core/shared-ui/react/bindings/` must come out byte-identical.
- **Golden fixtures.** `tests/golden` serves both the config catalog and the export; it moves with the catalog and the export crate
  reads it from there.
- **Flat public paths.** `lib.rs` re-exports modules as `helix_shell_core::audio` and so on, and 60 or more call sites use those
  names. The split must not force a rename on day one.

## Plan

1. **Facade first.** Create the crates and `git mv` the files. Keep `helix-shell-core` as a thin crate whose `lib.rs` is only
   `pub use` lines pointing at the new crates, so every downstream `use helix_shell_core::...` is unchanged. Inside each new crate, a
   `pub use helix_display as display;` style alias keeps the existing `crate::display::...` paths working. Verification: clippy
   `-D warnings`, all unit tests, both golden tests, empty diff on regenerated bindings, the integration tests.
2. **Switch consumers** one binary at a time to depend on only the crates they use, and delete the facade re-exports they no longer
   need. This is where the incremental-build gain is realised.
3. **Retire the facade** when nothing imports it.

## Estimate

| Step | Effort | Risk |
|---|---|---|
| Cut the five seams | 2 to 3 hours | Low: small, local, compiler-checked |
| Create 8 crates, move 83 files, fix the compile-time paths and bindings script | 1 day | Medium: mostly mechanical, but `include_str!` and ts-rs only fail at build or generate time |
| Switch consumers and prune re-exports | half a day | Low |
| Total | about 2 days, in 3 commits that each build and pass tests | |

Decision points before starting: whether `helix-app-models` should split per app (Mobius chat, Software, Monitor, assistant session
are independent of each other), and the crate names.
