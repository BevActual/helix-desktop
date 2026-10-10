# Session shell

Product shape (greeter / compositor / shell / services): `product-structure.md`.

**Live session shell:** Tauri `shell/helix-shell-tauri`.

```sh
tools/switch-shell.sh install   # links + units; starts shell only if not already running
tools/switch-shell.sh restart   # bounce the shell — can disrupt Cursor; prefer a plain terminal
tools/switch-shell.sh status
tools/iterate.sh                # fast rebuild + unit drop-in; no restart unless --restart
```

`install` / `iterate` never restart a live shell (restarting layer-shell has crashed Cursor on this session).

## Retired (removed)

| Piece | Status |
|-------|--------|
| C++ session UI `src/shell/` + `helix-shell` binary | **Deleted.** Live path is Tauri only. |
| GTK Settings / Software / System Monitor | **Deleted.** Replaced by `apps/helix-*`. |
| `helix-shell-dev.service` / `switch-shell native` | Removed. |

## What still lives in C++ (migrating to Rust)

| Area | Location | Notes |
|------|----------|-------|
| Desktop service | `src/desktop/`, `helix-service` | D-Bus authority (`org.helix.Desktop`); Rust target `services/helix-desktop` |
| Inference / assistant | `src/inference/`, `src/assistant/` | Separate processes; target `services/helix-inference`, `services/helix-assistant` |
| Platform libraries | `src/config`, `theme`, `dbus`, … | Move with each domain cutover |

See `product-structure.md` and `cpp-to-rust.md`.

## Day-to-day layout

```
Cargo.toml            Rust workspace (repo root; excludes compositor)
greeter/              Tauri login greeter (helix-greeter)
compositor/           helix-niri fork (own Cargo workspace)
core/                 pure models + desktop-api + shared-ui
shell/                live session shell (Tauri)
apps/                 Settings / Software / Monitor / Chat
services/             wallpaper + idle + helix-desktop (Rust service scaffold)
dev/                  nested probes only
target/               cargo output (fast/release)
protocols/            D-Bus + IPC contracts (source of truth)
tools/                switch-shell, nested e2e, helpers
src/desktop/          helix-service (keep)
src/assistant/        assistant process (→ services/helix-assistant)
src/inference/        model runtime (→ services/helix-inference)
assets/               icons, templates, translations
docs/design/          architecture + HIG; see README.md; archive/ is retired dumps
```

Design index: `docs/design/README.md`. Apps: `software-tauri.md`, `system-monitor-tauri.md`, `tauri-rust-shell.md`, `settings-tauri.md`, `greeter-tauri.md`.
Product names: `helix-identity.md`. Workspace notes: `rust-workspace.md`.
