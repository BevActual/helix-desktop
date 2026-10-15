# Desktop behavioural contract tests

`org.helix.Desktop` behaviour is checked in layers. **Rust `contract/`** is the bus integration harness; see `docs/design/cpp-to-rust.md` for migration context.

| Harness | Path | Role |
|---|---|---|
| **Rust contract** | `services/helix-desktop/tests/contract/`, `tools/nested/helix-desktop-configuration-contract.sh` | `helix-desktop-api`; one `helix-desktop` child; `helix-fake-bluez` |

## Retained checklist (product bar)

Use this list when adding tests. **Do not** revive the old GTK-era 1k-line C++ monolith.

### Shell (Tauri quick CC + bar)

| Check | Where covered |
|---|---|
| Service up; Configuration snapshot (`helix-desktop` false until register) | `contract/bootstrap.rs`, config-smoke |
| Audio meter lease guard | `contract/bootstrap.rs` |
| Network/Bluetooth snapshots; connect error path | `contract/bootstrap.rs` |
| Bluetooth pair + cancel + backend disconnect | `contract/bluetooth.rs` + `helix-fake-bluez` |
| `theme.mode` / `nightlight.enabled` via Configuration (quick tiles) | `contract/configuration.rs`, `helix-shell-core::desktop::quick` tests |
| `RegisterDesktop` + gamma / nightlight guards | `contract/configuration.rs` |
| Volume and domain `Action` in UI | `tools/nested/ui-suite.sh` (`qs`, `net`, `bt`, `cc`) |
| `RegisterDesktop` / confirmer on panels | Shell integration; agent tests for confirm path |

### Settings app + catalog

| Check | Where covered |
|---|---|
| Portable vs desktop-only gating (`GetSettings` pages pre/post register) | `contract/bootstrap.rs` |
| Scalar `Update` / `Reset` validation (representative rows) | `contract/configuration.rs` |
| Ordered-list CAS (dock pins, bar widgets, keybinds, notification rules) | `contract/configuration.rs` |
| `CommitOverrides` reject | `contract/configuration.rs` |
| Persistence across service restart | `contract/configuration.rs` (`run_persistence`) |
| Full catalog shape / defaults | `cargo test -p helix-config pristine_catalog_matches_golden`, `cargo test -p helix-config-export --test config_export_golden` |

### Agent / policy (not duplicated in `contract/`)

| Check | Where covered |
|---|---|
| Hello, Action, grants, manager | `tools/nested/helix-desktop-agent-integration.sh` |
| Risk policy units | `helix-shell-core::agent` tests |

### CI gates for “desktop works”

1. `tools/nested/helix-desktop-config-smoke.sh`
2. `tools/nested/helix-desktop-configuration-contract.sh`
3. `tools/nested/helix-desktop-agent-integration.sh`
4. Nested UI: `qs`, `net`, `bt`, `cc`, `settings` (via `ui-suite.sh`)

`tools/test-integration.sh` (also `just test-integration`) runs `helix_desktop_config_smoke` and `helix_desktop_configuration_contract` when `target/fast/helix-desktop` exists.

## Legacy (do not port)

- **`control_center.visible_tabs`** / `hidden_tabs` / nineteen-tab sidebar matrix
- **`control_center.shortcuts`** ordered-list torture (GTK quick CC layout)
- Exhaustive scalar sweeps (dock OSD, launcher, hot corners, clipboard policy, Wallhaven, SavePalette marathon)
- Per-row JSON type walk of the entire catalog (use goldens instead)

## Harness environment

Match nested runners: `HELIX_CONFIG_HOME`, `XDG_STATE_HOME`, private `dbus-run-session`, `DBUS_SYSTEM_BUS_ADDRESS` on the child (session bus alias for BlueZ/NM probes), `HELIX_DESKTOP_MOCK_AUDIO` + `HELIX_DESKTOP_MOCK_DISPLAYS`, **unset** `HELIX_STATE_HOME`, `XDG_DATA_DIRS=/var/empty`.

Seed: `harness::HELIX_TEST_CONFIG_TOML` (`theme.mode` only).

## Cleanup backlog (remaining)

| Item | Suggested action |
|---|---|
| **Catalog rows `control_center.visible_tabs` / shortcuts** | No Tauri CC/Settings consumers; still in schema, goldens, config smoke, user docs. Keep until product prune/re-home |
| **Inference image worker / llama native** | `helix-transcribe` is Rust; port `helix-image` and retire `native/inference/` llama (`cpp-to-rust.md`) |
| ~~**`src/` → `cpp/` rename**~~ | Done |
| **`cpp/desktop_api/client.*`** | Removed from `helix_core`; protocol + peer_identity remain for tests |

### Done (2026-10)

- C++ `desktop_service_test`, `remote_network`, `shell_tools`, `src/theme/` tree
- `desktop_migration_inventory` meson test; `config_export_main.cpp`
- C++ theme helper bins / `HELIX_DISABLE_CPP_THEME_HELPERS`
- Golden test rename: `pristine_catalog_matches_golden`
- `src/theme/` tree, `src/desktop_api/client.*`, `inetwork_service.h`, `dns_override.h`
- `config_service` no longer proxies runtime state over D-Bus (local `state_store` only)
- Contract harness: `configuration_extended.rs` folded into `configuration.rs`
- Dead `OverrideCommitHandler` / D-Bus override commit branch in `config_service`
- Config export: Rust-only `export_effective_toml`; C++ `helix-config-export` dropped from tests and `gen_config_export_*` scripts
- `helix-desktop` dead-code cleanup (agent hub, audio meter, NM constants, day_night test-only helper)
- C++ agent tree deleted (`cpp/desktop/`); agent Meson tests removed (Rust `helix-shell-core::agent`)
- C++ inference unit tests deleted (queue/routing/image/schema/llama harness)
- `helix-transcribe` and `helix-image` are Cargo; llama glue under `services/helix-inference/native/`
