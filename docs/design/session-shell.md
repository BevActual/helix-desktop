# Session shell

Product shape (greeter / compositor / shell / services): `product-structure.md`.

**Live session shell:** Tauri `shell/helix-shell-tauri`.

```sh
tools/switch-shell.sh install          # links + units; starts shell only if not already running
tools/switch-shell.sh restart-panels   # bounce Overlay panels only — safe while Cursor is open
tools/switch-shell.sh restart-bar      # bounce painted bar; exclusive-zone strut stays mapped
tools/switch-shell.sh restart-strut    # bounce strut — flaps niri work area; avoid with Cursor open
tools/switch-shell.sh restart          # bounce bar+panels (strut stays)
tools/switch-shell.sh status
tools/iterate.sh                       # fast rebuild + unit drop-in; no restart unless --restart
```

`install` / `iterate` never restart a live shell. Prefer `restart-panels` for UI work. `restart-bar` leaves `helix-shell-strut` (top + dock exclusive zones) and panels up. Cursor crashes came from niri calling `output_resized` on **every** layer destroy — helix-niri now skips that when the usable zone is unchanged; still avoid `restart-strut` while Cursor is open.

Split processes: `helix-shell-strut` (exclusive zone) + `helix-shell-bar` (`HELIX_SHELL_ROLE=bar`) + `helix-shell-panels` (`HELIX_SHELL_ROLE=panels`). Strut is its own tiny binary; bar and panels share `helix-shell-tauri`. Legacy monolith unit `helix-shell-tauri.service` remains for nested tests / rollback.

## Retired (removed)

| Piece | Status |
|-------|--------|
| C++ session UI `src/shell/` + `helix-shell` binary | **Deleted.** Live path is Tauri only. |
| GTK Settings / Software / System Monitor | **Deleted.** Replaced by `apps/helix-*`. |
| `helix-shell-dev.service` / `switch-shell native` | Removed. |

## Who owns what now

| Area | Location | Notes |
|------|----------|-------|
| Desktop authority | `services/helix-desktop` | `org.helix.Desktop`; unit `helix-service.service` runs `helix-desktop` |
| Assistant / inference | `services/helix-assistant`, `services/helix-inference` | Rust; `helix-transcribe` and `helix-image` are Cargo bins over C/C++ engine shims in `native/` |
| Theme | `core/helix-theme` + `helix-desktop` | Runtime theme and template apply are Rust |
| Contract tests | `services/helix-desktop/tests/contract/` | Rust bus harness; `helix-fake-bluez`; see `desktop-contract-tests.md` |

See `product-structure.md` and `cpp-to-rust.md`.

## Day-to-day layout

```
Cargo.toml            Rust workspace (repo root; excludes compositor)
greeter/              Tauri login greeter (helix-greeter)
compositor/           helix-niri fork (members of the root Cargo workspace)
core/                 pure models + desktop-api + shared-ui
shell/                live session shell (Tauri)
apps/                 Settings / Software / Monitor / Chat
services/             helix-desktop, assistant, inference, system-upgrade, wallpaper, idle
dev/                  nested probes only
target/               cargo output (fast/release)
protocols/            D-Bus + IPC contracts (source of truth)
tools/                switch-shell, nested e2e, helpers
services/helix-inference/  service + helix-transcribe + native/ llama|whisper
services/helix-image/      image worker + native/ C engine shim
assets/               icons, templates, translations
docs/design/          architecture + HIG; see README.md; archive/ is retired dumps
```

Design index: `docs/design/README.md`. Apps: `software-tauri.md`, `system-monitor-tauri.md`, `tauri-rust-shell.md`, `settings-tauri.md`, `greeter-tauri.md`.
Product names: `helix-identity.md`. Workspace notes: `rust-workspace.md`.
