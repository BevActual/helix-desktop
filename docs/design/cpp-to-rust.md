# C++ → Rust migration

Helix’s session UI is already Rust (Tauri). Remaining C++ owns desktop authority and helpers. This doc is the cutover map.

## Inventory

| C++ today | Rust home | Status |
|---|---|---|
| `helix-service` | `services/helix-desktop` (bin `helix-desktop`) | **Done.** Rust owns `org.helix.Desktop`; the C++ service was removed. Only `agent_policy`, `agent_confirmation_queue`, `notification_rules`, `shell_tools`, `tool_schema` remain in `src/desktop/` (shared with assistant/inference and the contract test) |
| `src/desktop/agent_*`, `tool_schema` | `helix-shell-core::agent` (+ service) | Risk policy, GrantStore, peer identity, Agent Surface Hello/Action/Confirm/manager |
| `src/config/`, theme, dbus adapters | service / shared crates | Port with each domain |
| `helix-assistant` | `services/helix-assistant` | After Agent Surface on Rust Desktop; keep separate from shell |
| `helix-inference` | `services/helix-inference` | Last; llama.cpp via FFI; sandboxed D-Bus activation |
| `helix-system-upgrade` | small helper or stay C++ | Low priority |
| `compositor/` | already Rust (own workspace) | Done |
| `greeter/` | Tauri scaffold | In progress |

## Cutover rules

1. **`protocols/` is authoritative.** D-Bus XML and IPC schemas stay the single source of truth. Rust checks or generates against them.
2. **One bus owner.** Never run two `org.helix.Desktop` processes. The installed unit (`helix-service.service`, name kept) and D-Bus activation run `helix-desktop` directly. `HELIX_SERVICE_BIN` names an explicit binary (nested harnesses use it). `just install` builds and installs `helix-desktop`.
3. **Parity before delete.** Nested or unit tests that cover a domain must pass against the Rust binary before that C++ domain is removed.
4. **Same change deletes C++.** When a domain cuts over, remove its C++ sources and meson bits in that change.

## Dev selection

```sh
cargo build --profile fast -p helix-desktop
HELIX_SERVICE_BIN=$PWD/target/fast/helix-desktop tools/nested/…
```

**CI vs Meson:** GitHub Actions runs Rust nested smokes (`helix-desktop-config`, `helix-desktop-audio-meter`, agent integration). The **helix-desktop Configuration1** job builds `helix-desktop` plus **`helix-template-apply`** / **`helix-theme-palette`** on Arch and runs `tools/nested/helix-desktop-config-smoke.sh` with `HELIX_*_BIN` set so `templates-apply` and **`Appearance1.SetAccent`** are exercised (not skipped). **Build (debug)** still runs full `just build`, install layout, and `meson test` (including `helix_desktop_config_smoke` when `target/fast/helix-desktop` exists). Meson builds `desktop_service_test` (C++, the behavioural contract) and runs it, plus the assistant/agent integration tests, against `target/fast|debug/helix-desktop` when that binary exists.

**Scalar catalog:** The C++ `settings_catalog.cpp` was removed; its rows live in Rust as hand-maintained `BOOTSTRAP` plus `settings_bootstrap_extra.rs`, checked against the frozen golden `core/helix-shell-core/tests/golden/settings_catalog_defaults.json`. Ordered types (key-chords, shortcuts, tabs, display lists, templates, transitions, notification rules, bar widget rows) stay in `ordered_lists.rs` / `ordered_catalog.rs`.

**Desktop-only catalog:** After `RegisterDesktop`, `GetSettings` also includes live `bar:<name>:<field>` scalars and `idle:<behavior>:<field>` rows from `config.toml` (`dynamic_desktop_catalog.rs`), matching C++ `settingsCatalog(helixDesktop=true)`. Override paths use `storage_path()` (`bar:*` → `bar.<name>.<field>`, `idle:*` → `idle.behavior.<name>.<field>`). `tools/nested/helix-desktop-config-smoke.sh` seeds a minimal bar + idle config and checks post-desktop rows plus `idle:*` `Update`.

**Soak before the flip:** `rust-desktop-soak.md` (`tools/soak/`) runs the live session on the Rust service and diffs snapshots against C++.

## Order

1. ~~Scaffold `helix-desktop` + risk policy in `helix-shell-core`~~
2. ~~Agent Surface (grants, Hello/Action/Confirm, ListTools, manager)~~ — Rust `Action` runs shell IPC via `helix_shell_core::shell_ipc`; `tests/desktop_agent_integration.py` and `desktop_agent_manager_integration.py` pass against `helix-desktop` (`tools/nested/helix-desktop-agent-integration.sh`, CI job `helix-desktop-agent`)
3. ~~Configuration1 bootstrap catalog + writes~~ (theme/audio/accessibility/shell chrome; growing)
4. ~~Audio1 in-memory backend~~ · **PipeWire via `wpctl`** when WirePlumber is present (`HELIX_DESKTOP_MOCK_AUDIO=1` for nested smoke); **`StartInputMeter` / `StopInputMeter` / `InputLevel`** via `audio_meter` + PipeWire capture (`Helix microphone level`, 8s lease, 80ms emit) · ~~Appearance1 GetSnapshot stub~~
5. ~~Network1 in-memory mock~~ · **NetworkManager** on system bus when present (`HELIX_DESKTOP_MOCK_NETWORK=1` for nested); **SecretAgent** + `Prompt`/`Reply`; **enterprise**, **SetDns**, **wired** / **cellular** actions; async `Completed`
6. ~~Bluetooth1 in-memory mock~~ · **BlueZ** when present (`HELIX_DESKTOP_MOCK_BLUETOOTH=1` for nested); **Agent1** + `Prompt`/`Reply`; **trust**, **cancel-pair**
7. ~~Displays1 GetSnapshot stub~~ · **niri** `msg --json outputs` when `NIRI_SOCKET` is set and desktop is registered (`HELIX_DESKTOP_MOCK_DISPLAYS=1` for nested)
8. ~~Appearance1 builtin palette-previews + SetWallpaper path~~ (no Material expand / wallhaven / apply yet)
9. ~~Configuration1 RegisterCapabilities + Get/Set/ClearState + catalog growth~~ · **`UpdateOrderedList`** + **`CommitOverrides`** with ordered-list catalog parity (**application-list**, **key-chords**, **tab-list**, **shortcut-list**, **notification-rules**, **display-list**, **template-list**, **transition-list**, **widget-list** / `bar:*`); monitor pickers take live compositor output names; community template list options read `state/community-templates/catalog.json` + cache dirs; bootstrap scalar catalog ~320+ rows (hand + generated) plus desktop **bar** / **idle** extensions; validation aligned with C++ for hooks, palettes, nightlight pairing, Mobius `ai.*` mirror keys
10. ~~AgentSurfaceActivity1~~ (desktop-only kill switch, pending, grants view)
11. ~~Community/custom palette-previews~~ + ~~opt-in catalog HTTP sync~~ (`HELIX_COMMUNITY_PALETTE_CATALOG_URL`)
12. ~~UpdateApplicationList for dock/launcher pins~~ (remaining ordered-list catalog rows still stub)
13. Real backends (PW/NM/BlueZ/niri) · ~~Configuration1 `Update`/`Reset` guards~~ · ~~community template catalog sync~~ (`HELIX_COMMUNITY_TEMPLATE_CATALOG_URL` via `RefreshPalettes`) · ~~**Appearance1 `Reapply`** + **`templates-apply` msg IPC**~~ (`msg_ipc.rs`; **`helix-template-apply`** via `HELIX_TEMPLATE_APPLY_BIN`; override `HELIX_TEMPLATE_APPLY_CMD`) · ~~**Appearance1 `SavePalette` / `SetRole` / `SetAccent`**~~ (`theme_palette.rs`: in-process via `core/helix-theme` for builtin/custom sources in dark/light mode, with C++ goldens under `core/helix-theme/tests/golden/`; falls back to **`helix-theme-palette`** via `HELIX_THEME_PALETTE_BIN` / `HELIX_THEME_PALETTE_CMD` for wallpaper/community sources and `auto` mode) · ~~system-bus agent unregister on shutdown~~ · ~~display-list options from live compositor inventory~~ · ~~config smoke Meson helper parity in CI~~ · ~~in-process template apply~~ (`core/helix-theme`: render, plan/execute, HCT, palette transforms, custom colors; C++ goldens in `core/helix-theme/tests/golden/`; falls back to **`helix-template-apply`** for wallpaper/community palette sources, `auto` mode, `post_action` (KDE/Firefox), templates reading `config.*`/`image`) · full in-process theme apply (no C++ helper) · ~~Wallhaven~~ (`services/helix-desktop/src/wallhaven.rs`: bounded per-client search/thumbnail/download jobs, host-restricted URLs, C++ client goldens) · remaining domain cutovers
14. Assistant, then inference
