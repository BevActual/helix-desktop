# Rust `config_export` port

Theme templates read `config.*` from `config_export::serialize` output (Rust in-process; `helix-rust-config-export` for fixture regeneration).

## Parity gates

| Gate | Command / artifact |
|------|-------------------|
| Minimal fixture byte match | `cargo test -p helix-shell-core --test config_export_golden` |
| Parity fixtures (no C++ at test time) | `tests/golden/export_fixtures/*.toml` via `tools/gen_config_export_fixtures.py` |
| Regenerate golden | `tools/gen_config_export_golden.sh` |
| Verify golden fresh | `tools/verify_config_export_golden.sh` |
| Schema read/write inverses | `meson test config_schema_roundtrip` (`tests/config_schema_roundtrip_test.cpp`) |

## C++ sources of truth

- Merge + parse: `ConfigService::buildEffectiveConfigFromSources` (`src/config/config_service.cpp`)
- Serialize: `config_export::serialize` (`src/config/config_export.cpp`)
- Section list: `helix::config::schema::sections()` (`src/config/schema/config_sections.cpp`)
- Extra root tables: `bar`, `widget`, `desktop_widgets`, `lockscreen_widgets`

## Rust implementation plan

1. **`helix-shell-core::desktop::config_export::rust_export_toml`** — wired ahead of C++ in `export_effective_toml`. Default export tree comes from `config_export_defaults::assemble_default_export` (not an embedded golden TOML at runtime). Patch merged user TOML leaves when every merged leaf path exists in that tree (or is under an extra `bar.<name>` / `widget.<name>` table, or is an optional schema leaf under an exported ancestor such as `shell.panel.glass`, or under `shell.launcher.providers.<name>` seeded with C++ provider defaults); merged keys under top-level tables absent from export (e.g. `plugins`) are ignored like C++ parse; wallpaper runtime keys and legacy `calendar.account.*.url` are parse-only; dynamic `calendar.account.<id>` tables are seeded with schema export defaults; `desktop_widgets.widget.<id>` / `lockscreen_widgets.widget.<id>` placements are seeded with C++ placement defaults. Otherwise `None` unless the caller passes an explicit `export_bin` (tests/tools only). Extra bar names are seeded from exported `bar.default` defaults; per-monitor overrides clone the resolved bar table (without `position`) plus `match`; extra widgets clone an export template by `type`; named idle behaviors trim `behavior_order` and drop unmentioned behaviors before leaf patch; `config_export_serialize::finalize_serialized_export` sorts `widget`, `calendar.account`, and placement widget maps, strips false placement flip flags, normalizes `keybinds` chord strings, omits empty `wallpaper.default.path`, and rounds floats to `f32` like C++. Empty merged tables (e.g. `[hooks]`) no longer wipe schema defaults during patch. Integration tests in `config_export_golden.rs` compare Rust vs C++ across theme, dock, notifications, includes, bar layouts, launcher providers, optional panel glass, and legacy widget keys; unit test `example_toml_is_rust_export_patchable` guards the shipped `example.toml`; `declines_unknown_merged_paths` guards legacy keys C++ ignores (e.g. `shell.list`).
2. Port **merge + `normalizeLegacyConfig` + `parseConfigTable`** into Rust (or generate from schema), starting with sections templates touch most (`theme`, `shell`, `bar`). **`mergeConfigWithIncludes`** lives in `desktop/config_merge.rs` and backs `ConfigStore` base config load. **`settings.toml` sidecar migrations** and **`normalizeLegacyConfig`** (`config_settings_migrate.rs`) run in `ConfigStore::merged_for_config_export` before export; keep `CURRENT_CONFIG_VERSION` aligned with `config_migrations.cpp`. Public API: [`serialize_effective_export`](../../core/helix-shell-core/src/desktop/config_export.rs) (patch today).
3. **Milestone B done:** [`serialize_default_config`](../../core/helix-shell-core/src/desktop/config_export_serialize.rs) = `assemble_default_export` + finalize; matches `tests/golden/config_export_minimal.toml`. **Milestone C (in progress):** all `SCHEMA_EXPORT_SECTIONS` export through [`config_export_typed`](../../core/helix-shell-core/src/desktop/config_export_typed.rs) + [`config_export_typed_scalar`](../../core/helix-shell-core/src/desktop/config_export_typed_scalar.rs) (structs for `storage`, `accessibility`, `battery`, `theme`, `calendar`, `nightlight`, `weather`, `tailscale`, `location`, `hooks`, `audio`, `backdrop`, `brightness`, `hot_corners`, `idle` scalar fields, `notification`, `osd`; filtered merge for the rest); `idle.behavior` / `behavior_order`, `bar` / widget / placement roots, and dynamic passes follow. Next: `shell`, `wallpaper`, `dock`, `keybinds`, `lockscreen`, `system`, `control_center`.
4. Port **bar/widget placement** emitters from `config_export.cpp` anonymous namespace.
5. When `config_export_golden` passes **without** `HELIX_CONFIG_EXPORT_BIN`, remove C++ export from Meson/CI. **Done:** frozen fixtures, in-process runtime export, `helix-rust-config-export` (`cargo build -p helix-shell-core --bin helix-rust-config-export`) for `tools/gen_config_export_*`; C++ `helix-config-export` target removed from Meson. Optional: keep `src/config/config_export_main.cpp` only until `parseConfigTable` → serialize replaces leaf-patch.

`just build` compiles `helix-rust-config-export` via Cargo alongside Meson theme helpers.
