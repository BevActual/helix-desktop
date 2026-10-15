# The core crates (the split of helix-shell-core)

Done on 2026-10-15. `core/helix-shell-core` held 83 files and 36k lines: the settings catalog and config export, app and device view
models, agent policy and the shell's surface models. Every service and app depended on all of it, so an edit anywhere rebuilt
everything that linked it. It is now ten focused crates and the old crate is gone.

## The crates

| Crate | Holds | Lines | Used by |
|---|---|---|---|
| `helix-base` | display text sanitising, panel ids, window facts, `nested_test_session` | ~250 | everything below |
| `helix-agent` | risk tiers, grants, confirmation, the assistant session, tool schema | ~4.9k | desktop service, assistant, shell, Settings, Mobius |
| `helix-config` | settings catalog, ordered lists, shell prefs, migration, `inference_routing` | ~10.2k | desktop service, Settings, Mobius, shell, strut |
| `helix-config-export` | effective config export, and the `helix-rust-config-export` binary | ~5.9k | desktop service |
| `helix-models` | audio, network, Bluetooth, brightness, power, pairing, quick settings, wallhaven | ~3.1k | shell, Settings |
| `helix-compositor` | compositor config and IPC, capture, shortcuts, niri, agent desk | ~1.5k | shell, Settings, Mobius |
| `helix-shell-model` | bar and panel surfaces, dock, tray, launcher, notifications, island, panels IPC | ~6.6k | shell, strut |
| `helix-chat` | Mobius: chat, personas, lab, model catalog, markdown | ~2.7k | Mobius, shell |
| `helix-software-model` | Helix Software catalog, search, jobs | ~0.6k | Software |
| `helix-monitor-model` | Helix System Monitor `/proc` models | ~0.4k | System Monitor |

Dependency order, with no back-edges: `helix-base` ← every other crate; `helix-compositor` ← `helix-shell-model`;
`helix-config` ← `helix-config-export`, `helix-chat`.

## What it bought

Editing `core/helix-chat/src/chat.rs`, which only Mobius and the shell use, then re-checking each package alone (`cargo check`;
real builds and tests cost more):

| Package | Before the split | After |
|---|---|---|
| `helix-desktop` | 13.8 s | 0.13 s |
| `helix-assistant` | 5.2 s | 0.11 s |
| `helix-software` | 6.0 s | 0.67 s |
| `helix-mobius`, `helix-shell-tauri` (use chat) | n/a | 2.1 s, 3.3 s |

## What had to change

- **The seams.** `PANELS` and `WindowFact` moved to `helix-base` (`shell_surface` and `dock` re-export them). The `agent_desk` and
  `compositor_config` modules, which used each other, share `helix-compositor`. `inference_routing` sits beside the settings that
  validate it. `ConfigStore::merged_for_config_export` became `pub` because the export crate is its caller.
- **Paths.** `include_str!` paths into `protocols/` are one directory shallower, goldens live in the crate that owns them
  (`helix-agent/tests/golden`, `helix-config/tests/golden`, `helix-config-export/tests/golden`), and `gen-bindings.sh` runs the five
  crates that export TypeScript types.
- **Names inside a crate.** Each new `lib.rs` re-exports the modules it uses from sibling crates (`pub use helix_base::display;`), so
  `crate::display::...` kept working inside moved files.

## Rules for the core crates

- A new model goes in the crate of the thing it models. If it fits none, ask whether it is shared (then `helix-base`, and only if it
  has no dependencies) or whether it is a new crate.
- A core crate depends only on `helix-base` and the crates above it in the order given. `tools/check-layers.py` enforces both the
  directory direction and the `[crates]` table in `tools/layers.toml`; a new core crate must be added there.
- A consumer lists only the crates it uses. If a binary starts needing a crate it did not have, that is a change worth noticing.
- No D-Bus servers, no UI toolkit, no I/O beyond reading files in these crates. They are the part of Helix that is easy to test.

## Verified

`cargo clippy --workspace --all-targets -- -D warnings` clean; 565 unit and golden tests before and 565 after; regenerated
TypeScript bindings byte-identical; `tools/verify_config_export_golden.sh` passes; the integration suite is unchanged.
