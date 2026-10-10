# Helix Rust workspace

Product layout and runtime roles: `product-structure.md`.

Domain layout under `core/` / `shell/` / `greeter/` / `apps/` / `services/`:

| Dir | Role |
|---|---|
| `core/` | `helix-shell-core` (pure models), `helix-desktop-api`, `shared-ui` |
| `shell/` | `helix-shell-tauri` — live session shell |
| `greeter/` | `helix-greeter` — greetd login (Tauri) |
| `apps/` | Settings, Software, System Monitor, Chat (Tauri toplevels) |
| `services/` | `helix-wallpaper`, `helix-idle`, `helix-desktop` (Rust `org.helix.Desktop` scaffold) |
| `dev/` | Nested probes only (`helix-attacker`, `experiments/`) |
| `compositor/` | helix-niri fork (own Cargo workspace; excluded from this one) |

**The session shell is Tauri** (`tools/switch-shell.sh install`). C++ session UI and GTK apps are deleted; `helix-service` stays in C++ under `src/desktop/`.

## Build

```sh
export PATH="$HOME/.cargo/bin:$PATH"
cargo test
cargo clippy -- -D warnings
cargo build --release -p helix-shell-tauri -p helix-greeter -p helix-settings -p helix-software -p helix-system-monitor
cargo build --profile fast -p helix-shell-tauri -p helix-greeter
```

Binaries still land in `target/{fast,release}/` (workspace target dir unchanged).

Design notes: `docs/design/software-tauri.md`, `docs/design/system-monitor-tauri.md`, `docs/design/session-shell.md`, `docs/design/greeter-tauri.md`.

## Live session

`tools/switch-shell.sh install` points user units at `target/release` or `fast` binaries. Nested tests never touch the live session:

```sh
HELIX_NESTED_SCRIPT=tools/nested/assistant-flow-test.sh \
  tools/nested/run.sh flow tools/nested/tauri-shell.sh
```
