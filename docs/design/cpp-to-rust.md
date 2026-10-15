# C++ → Rust migration

> **Status: complete (2026-10-15).** The `cpp/` tree is deleted and Rust owns the desktop. This page is now the record of how
> the cutover was done, plus the two items that are still open: the llama/whisper/image native shims and the greeter's remaining
> features. Current structure lives in `product-structure.md`.

Helix’s session UI is already Rust (Tauri). **`org.helix.Desktop` is Rust** (`services/helix-desktop`). **Inference is a Rust product** (`services/helix-inference`). Runtime theme is Rust (`helix-theme`). **The `cpp/` tree is deleted**; the only remaining non-Rust code is Cargo-built native glue: the llama C++ under `services/helix-inference/native/inference/` and the C shims (`image_engine.c`, `transcribe_engine.c`). This doc is the cutover map and backlog.

## Inventory

| C++ today | Rust home | Status |
|---|---|---|
| `helix-service` | `services/helix-desktop` (bin `helix-desktop`) | **Done.** Rust owns `org.helix.Desktop`; the C++ service was removed. |
| `cpp/desktop/agent_*`, `tool_schema` | `helix-shell-core::agent` (+ service) | **Done.** C++ agent tree deleted; Rust owns risk policy, GrantStore, confirmation queue, tool schema |
| `cpp/config/` | `helix-shell-core` | **Done.** C++ config library removed. Export, migrations, and settings validation live in Rust (`config_export_*`, `config_settings_migrate`, `settings.rs`) |
| `helix-assistant` | `services/helix-assistant` | **Done.** Rust; reply/step logic and the tool schema live in `helix-shell-core::agent` (`assistant_core`, `tool_schema`), checked against C++ goldens. `just install` installs the binary |
| `helix-inference` | `services/helix-inference` | **Mostly done (Rust).** Service, queue, routing, `helix-transcribe`, and `helix-image` are Cargo. Llama glue is still C++ under `services/helix-inference/native/inference/`. The image engine shim is C (`services/helix-image/native/image_engine.c`). Whisper stays a C shim (`native/transcribe_engine.c`) |
| `helix-system-upgrade` | `services/helix-system-upgrade` | **Done.** Same plan logic and tests; `just install` puts it in libexecdir where the polkit policy points |
| `compositor/` | already Rust (own workspace) | Done |
| `greeter/` | Tauri scaffold | In progress |

## Cutover rules

1. **`protocols/` is authoritative.** D-Bus XML and IPC schemas stay the single source of truth. Rust checks or generates against them.
2. **One bus owner.** Never run two `org.helix.Desktop` processes. The installed unit (`helix-service.service`, name kept) and D-Bus activation run `helix-desktop` directly. `HELIX_SERVICE_BIN` names an explicit binary (nested harnesses use it). `just install` builds and installs `helix-desktop`.
3. **Parity before delete.** Nested or unit tests that cover a domain must pass against the Rust binary before that C++ domain is removed.
4. **Same change deletes C++.** When a domain cuts over, remove its C++ sources in that change.

## Dev selection

```sh
just build debug   # helix-rust-config-export (Cargo). `just build-theme-golden` for Rust template golden regen.
cargo build --profile fast -p helix-desktop
HELIX_SERVICE_BIN=$PWD/target/fast/helix-desktop tools/nested/…
```

**CI:** GitHub Actions runs Rust nested smokes (`helix-desktop-config`, `helix-desktop-audio-meter`, agent integration). The **helix-desktop Configuration1** job runs `helix-desktop-config-smoke.sh` and **`helix-desktop-configuration-contract.sh`**; `tools/test-integration.sh` mirrors those when `target/fast/helix-desktop` exists. Template apply and palette paths are in-process `helix-theme` only (`HELIX_TEMPLATE_APPLY_CMD` / `HELIX_THEME_PALETTE_CMD` optional test hooks). Template golden regen: **`cargo run -p helix-theme --bin gen-golden-template`** (`tools/regen_golden_template.sh`). **`helix-rust-config-export`** (Cargo) regenerates export golden/fixture TOML. **Build (debug)** runs `just build-services`, the install layout test, and `tools/test-integration.sh`, which also runs the assistant and agent integration tests against `target/fast|debug/helix-desktop` when present.

**Verification (desktop migration smoke):**

```sh
cargo test -p helix-theme
cargo test -p helix-shell-core pristine_catalog_matches_golden
cargo test -p helix-shell-core --test config_export_golden
cargo test -p helix-desktop --bin helix-desktop in_process
tools/nested/helix-desktop-configuration-contract.sh   # private dbus-run-session; unset HELIX_STATE_HOME (it overrides XDG_STATE_HOME for settings.toml)
tools/verify_config_export_golden.sh
tools/test-integration.sh layering template_undo_signal
```

**Scalar catalog:** The C++ `settings_catalog.cpp` was removed; its rows live in Rust as hand-maintained `BOOTSTRAP` plus `settings_bootstrap_extra.rs`, checked against the frozen golden `core/helix-shell-core/tests/golden/settings_catalog_defaults.json`. Ordered types (key-chords, shortcuts, tabs, display lists, templates, transitions, notification rules, bar widget rows) stay in `ordered_lists.rs` / `ordered_catalog.rs`.

**Wallpaper palette source:** `helix-theme::wallpaper` generates palettes from the wallpaper in Rust (112x112 resize, quantize and
score, the five Material schemes through the vendored `third_party/material-colors`, the five custom HSL schemes), checked against
frozen goldens (60 palettes, 8 resized buffers, 4 template-apply scenarios). The service uses it for template apply and
save-palette / set-role / set-accent. Edge cases that cannot resolve in-process (unreadable wallpaper, missing community cache when
offline, etc.) return errors to the caller.

**Desktop-only catalog:** After `RegisterDesktop`, `GetSettings` also includes live `bar:<name>:<field>` scalars and `idle:<behavior>:<field>` rows from `config.toml` (`dynamic_desktop_catalog.rs`), matching C++ `settingsCatalog(helixDesktop=true)`. Override paths use `storage_path()` (`bar:*` → `bar.<name>.<field>`, `idle:*` → `idle.behavior.<name>.<field>`). `tools/nested/helix-desktop-config-smoke.sh` seeds a minimal bar + idle config and checks post-desktop rows plus `idle:*` `Update`.

**Session soak:** `rust-desktop-soak.md` (`tools/soak/`) is a hardware checklist for NM/BlueZ/PW/niri with the live shell; use it after large desktop changes.

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
12. ~~UpdateApplicationList for dock/launcher pins~~ · ~~ordered-list catalog rows~~ (`ordered_lists.rs` / `ordered_catalog.rs`; config smoke + Rust `contract/`)
13. ~~Real backends (PW/NM/BlueZ/niri)~~ · ~~Configuration1 guards, community catalogs, Appearance1 theme paths, Wallhaven, in-process theme (`helix-theme`; legacy C++ theme tree removed)~~ · ~~Desktop bus contract (`contract/`, C++ `desktop_service_test` retired)~~ · **Open:** inference C++ tail (workers + llama shim → Rust), greeter
14. ~~Assistant~~ (Rust: `services/helix-assistant`), ~~inference service shell~~ (Rust: `services/helix-inference`; **finish** workers + engine in Rust), ~~system-upgrade~~ (Rust: `services/helix-system-upgrade`)

## Next (C++ tail)

1. ~~**Theme C++ tail**~~ — **Done.** Runtime theme is Rust (`helix-theme`, `helix-desktop`). The C++ theme tree (and later all of `helix_core` and `cpp/`) is deleted. Goldens and contracts live under `cargo test -p helix-theme`; template regen uses `gen-golden-template`.
2. **Inference → Rust** — workers are Cargo. The image engine shim is C (`image_engine.c`). Replace the llama C++ under `native/inference/` (`llama_backend.cpp`, `schema_grammar.cpp`, `llama_shim.cpp`).
3. ~~**Desktop bus contract**~~ — **Done.** `services/helix-desktop/tests/contract/` + `tools/nested/helix-desktop-configuration-contract.sh`; checklist in `desktop-contract-tests.md`. C++ `desktop_service_test` removed.
4. **Greeter** — Appearance sync is in place (Settings **Sync login screen**, `helix-greeter-apply-appearance`, greeter paint). Still open: power actions, last account and session, packaging. See `greeter/HELIX.md`.
5. ~~**`src/` → `cpp/` rename**~~ — **Done**, then superseded: the `cpp/` tree itself is deleted, and Meson, which compiled nothing and only drove installs and tests, was replaced by `tools/install.sh` and `tools/test-integration.sh` (2026-10-15).
