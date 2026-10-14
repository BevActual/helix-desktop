# C++ → Rust migration

Helix’s session UI is already Rust (Tauri). Remaining C++ owns desktop authority and helpers. This doc is the cutover map.

## Inventory

| C++ today | Rust home | Status |
|---|---|---|
| `helix-service` | `services/helix-desktop` (bin `helix-desktop`) | Scaffold; C++ still owns the live bus name |
| `src/desktop/agent_*`, `tool_schema` | `helix-shell-core::agent` (+ service) | Risk policy, GrantStore, peer identity, Agent Surface Hello/Action/Confirm/manager |
| `src/config/`, theme, dbus adapters | service / shared crates | Port with each domain |
| `helix-assistant` | `services/helix-assistant` | After Agent Surface on Rust Desktop; keep separate from shell |
| `helix-inference` | `services/helix-inference` | Last; llama.cpp via FFI; sandboxed D-Bus activation |
| `helix-system-upgrade` | small helper or stay C++ | Low priority |
| `compositor/` | already Rust (own workspace) | Done |
| `greeter/` | Tauri scaffold | In progress |

## Cutover rules

1. **`protocols/` is authoritative.** D-Bus XML and IPC schemas stay the single source of truth. Rust checks or generates against them.
2. **One bus owner.** Never run two `org.helix.Desktop` processes. Nested harness / install picks C++ or Rust via `HELIX_SERVICE_BIN` (default: meson `helix-service`).
3. **Parity before delete.** Nested or unit tests that cover a domain must pass against the Rust binary before that C++ domain is removed.
4. **Same change deletes C++.** When a domain cuts over, remove its C++ sources and meson bits in that change.

## Dev selection

```sh
# C++ (default today)
meson compile -C build helix-service

# Rust scaffold
cargo build --profile fast -p helix-desktop
HELIX_SERVICE_BIN=$PWD/target/fast/helix-desktop tools/nested/…
```

**CI vs Meson:** GitHub Actions runs Rust nested smokes (`helix-desktop-config`, `helix-desktop-audio-meter`, agent integration) against `cargo build` binaries. Meson still builds `helix-service`, **`helix-template-apply`**, **`helix-theme-palette`**, and `desktop_service_test` (C++). When `target/fast/helix-desktop` or `target/debug/helix-desktop` exists, `meson test helix_desktop_config_smoke` runs `tools/nested/helix-desktop-config-smoke.sh` via `dbus-run-session` (prefers **fast**). If `_build/helix-template-apply` exists, the smoke sets `HELIX_TEMPLATE_APPLY_BIN` so `templates-apply` exercises C++ `TemplateEngine`. C++ `desktop_service` integration tests still use the meson `helix-service` binary unless a future gate keys off `HELIX_SERVICE_BIN`.

**Scalar catalog:** C++ `settings_catalog.cpp` rows that are not ordered-list types are mirrored in Rust via hand-maintained `BOOTSTRAP` plus generated `settings_bootstrap_extra.rs` (`python3 tools/gen-rust-settings-bootstrap.py` after C++ catalog edits). Ordered types (key-chords, shortcuts, tabs, display lists, templates, transitions, notification rules, bar widget rows) stay in `ordered_lists.rs` / `ordered_catalog.rs`.

**Desktop-only catalog:** After `RegisterDesktop`, `GetSettings` also includes live `bar:<name>:<field>` scalars and `idle:<behavior>:<field>` rows from `config.toml` (`dynamic_desktop_catalog.rs`), matching C++ `settingsCatalog(helixDesktop=true)`. Override paths use `storage_path()` (`bar:*` → `bar.<name>.<field>`, `idle:*` → `idle.behavior.<name>.<field>`). `tools/nested/helix-desktop-config-smoke.sh` seeds a minimal bar + idle config and checks post-desktop rows plus `idle:*` `Update`.

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
13. Real backends (PW/NM/BlueZ/niri) · ~~Configuration1 `Update`/`Reset` guards~~ · ~~community template catalog sync~~ (`HELIX_COMMUNITY_TEMPLATE_CATALOG_URL` via `RefreshPalettes`) · **Appearance1 `Reapply`** + **`templates-apply` msg IPC** (`msg_ipc.rs`; prefers Meson **`helix-template-apply`** when `HELIX_TEMPLATE_APPLY_BIN` / build dir / sibling of `helix-desktop` resolves; sets `HELIX_ASSETS_DIR` from repo `assets/` in dev; else niri `LoadConfigFile` + built-in `apply.sh` post-hooks; override via `HELIX_TEMPLATE_APPLY_CMD`) · **Appearance1 `SavePalette` / `SetRole` / `SetAccent`** via **`helix-theme-palette`** (`theme_palette.rs`; `HELIX_THEME_PALETTE_BIN` / sibling of `helix-desktop`; override via `HELIX_THEME_PALETTE_CMD`) · full in-process theme apply · Meson/Rust nested parity for remaining domains
14. Assistant, then inference
