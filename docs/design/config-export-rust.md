# Rust `config_export` port

Theme templates read `config.*` from `config_export::serialize` output (C++ today via `helix-config-export`).

## Parity gates

| Gate | Command / artifact |
|------|-------------------|
| Minimal fixture byte match | `cargo test -p helix-shell-core --test config_export_golden` |
| Regenerate golden | `tools/gen_config_export_golden.sh` |
| Verify golden fresh | `tools/verify_config_export_golden.sh` |
| Schema read/write inverses | `meson test config_schema_roundtrip` (`tests/config_schema_roundtrip_test.cpp`) |

## C++ sources of truth

- Merge + parse: `ConfigService::buildEffectiveConfigFromSources` (`src/config/config_service.cpp`)
- Serialize: `config_export::serialize` (`src/config/config_export.cpp`)
- Section list: `helix::config::schema::sections()` (`src/config/schema/config_sections.cpp`)
- Extra root tables: `bar`, `widget`, `desktop_widgets`, `lockscreen_widgets`

## Rust implementation plan

1. **`helix-shell-core::desktop::config_export::rust_export_toml`** — wired ahead of C++ in `export_effective_toml`. Default export tree comes from `config_export_defaults::assemble_default_export` (not an embedded golden TOML at runtime). Patch merged user TOML leaves when every merged leaf path exists in that tree (or is under an extra `bar.<name>` / `widget.<name>` table); merged keys under top-level tables absent from export (e.g. `plugins`) are ignored like C++ parse; wallpaper runtime keys and legacy `calendar.account.*.url` are parse-only; dynamic `calendar.account.<id>` tables are seeded with schema export defaults; `desktop_widgets.widget.<id>` / `lockscreen_widgets.widget.<id>` placements are seeded with C++ placement defaults. Otherwise `None` and C++ runs. Extra bar names are seeded from exported `bar.default` defaults; per-monitor overrides clone the resolved bar table (without `position`) plus `match`; extra widgets clone an export template by `type`; named idle behaviors trim `behavior_order` and drop unmentioned behaviors before leaf patch; `config_export_serialize::finalize_serialized_export` sorts `widget`, `calendar.account`, and placement widget maps and strips false placement flip flags like C++. Integration tests in `config_export_golden.rs` compare Rust vs C++ across theme, dock, notifications, includes, bar layouts, and legacy widget keys; unit test `declines_unknown_merged_paths` guards legacy keys C++ ignores (e.g. `shell.list`).
2. Port **merge + `normalizeLegacyConfig` + `parseConfigTable`** into Rust (or generate from schema), starting with sections templates touch most (`theme`, `shell`, `bar`). **`mergeConfigWithIncludes`** lives in `desktop/config_merge.rs` and backs `ConfigStore` base config load. **`settings.toml` sidecar migrations** and **`normalizeLegacyConfig`** (`config_settings_migrate.rs`) run in `ConfigStore::merged_for_config_export` before leaf patch; keep `CURRENT_CONFIG_VERSION` aligned with `config_migrations.cpp`.
3. Port **`writeTable` per section** in the same order as `config_sections.cpp` (`config_export_defaults.rs`: section list + all schema sections plus `bar` / `widget` / placement roots via `assemble_default_export` (golden file remains the C++ drift check in `tools/verify_config_export_golden.sh` and unit tests).
4. Port **bar/widget placement** emitters from `config_export.cpp` anonymous namespace.
5. When `config_export_golden` passes **without** `HELIX_CONFIG_EXPORT_BIN`, remove `helix-config-export` from Meson/CI.

Until step 5, `just build debug` (or install layout) must ship `helix-config-export` next to `helix-desktop` or under `build-debug/`.
