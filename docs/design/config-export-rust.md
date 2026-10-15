# Rust `config_export` port

Theme templates read `config.*` from `config_export::serialize` output (Rust in-process; `helix-rust-config-export` for fixture regeneration).

## Parity gates

| Gate | Command / artifact |
|------|-------------------|
| Minimal fixture byte match | `cargo test -p helix-config-export --test config_export_golden` |
| Parity fixtures (no C++ at test time) | `tests/golden/export_fixtures/*.toml` via `tools/gen_config_export_fixtures.py` |
| Regenerate golden | `tools/gen_config_export_golden.sh` |
| Verify golden fresh | `tools/verify_config_export_golden.sh` |
| Schema read/write inverses | `cargo test -p helix-config-export --test config_export_golden` |

## C++ sources of truth

- Merge + parse: `ConfigService::buildEffectiveConfigFromSources` (`cpp/config/config_service.cpp`)
- Serialize: `config_export::serialize` (`cpp/config/config_export.cpp`)
- Section list: `helix::config::schema::sections()` (`cpp/config/schema/config_sections.cpp`)
- Extra root tables: `bar`, `widget`, `desktop_widgets`, `lockscreen_widgets`

## Rust implementation plan

1. **`helix-shell-core::desktop::config_export::rust_export_toml`** — default export tree from `config_export_defaults::assemble_default_export` (not an embedded golden TOML at runtime). Patch merged user TOML leaves when every merged leaf path exists in that tree (or is under an extra `bar.<name>` / `widget.<name>` table, or is an optional schema leaf under an exported ancestor such as `shell.panel.glass`, or under `shell.launcher.providers.<name>` seeded with C++ provider defaults); merged keys under top-level tables absent from export (e.g. `plugins`) are ignored like C++ parse; wallpaper runtime keys and legacy `calendar.account.*.url` are parse-only; dynamic `calendar.account.<id>` tables are seeded with schema export defaults; `desktop_widgets.widget.<id>` / `lockscreen_widgets.widget.<id>` placements are seeded with C++ placement defaults. Extra bar names are seeded from exported `bar.default` defaults; per-monitor overrides clone the resolved bar table (without `position`) plus `match`; extra widgets clone an export template by `type`; named idle behaviors trim `behavior_order` and drop unmentioned behaviors before leaf patch; `config_export_serialize::finalize_serialized_export` sorts `widget`, `calendar.account`, and placement widget maps, strips false placement flip flags, normalizes `keybinds` chord strings, omits empty `wallpaper.default.path`, and rounds floats to `f32` like C++. Empty merged tables (e.g. `[hooks]`) no longer wipe schema defaults during patch. Integration tests in `config_export_golden.rs` compare Rust export to frozen fixtures across theme, dock, notifications, includes, bar layouts, launcher providers, optional panel glass, and legacy widget keys; unit test `example_toml_is_rust_export_patchable` guards the shipped `example.toml`; `declines_unknown_merged_paths` guards legacy keys C++ ignores (e.g. `shell.list`).
2. Port **merge + `normalizeLegacyConfig` + `parseConfigTable`** into Rust (or generate from schema), starting with sections templates touch most (`theme`, `shell`, `bar`). **`mergeConfigWithIncludes`** lives in `desktop/config_merge.rs` and backs `ConfigStore` base config load. **`settings.toml` sidecar migrations** and **`normalizeLegacyConfig`** (`config_settings_migrate.rs`) run in `ConfigStore::merged_for_config_export` before export; keep `CURRENT_CONFIG_VERSION` aligned with `config_migrations.cpp`. Public API: [`serialize_effective_export`](../../core/helix-config-export/src/config_export.rs) (patch today).
3. **Milestone B done:** [`serialize_default_config`](../../core/helix-config-export/src/config_export_serialize.rs) = `assemble_default_export` + finalize; matches `tests/golden/config_export_minimal.toml`. **Milestone C done:** every `SCHEMA_EXPORT_SECTIONS` table routes through [`export_typed_schema_section`](../../core/helix-config-export/src/config_export_typed.rs); [`config_export_typed_shell`](../../core/helix-config-export/src/config_export_typed_shell.rs) composes root scalars + typed overlays with `merge_export_subtree` for typed `launcher`, `session`, `screenshot` / `screen_record`, `keyboard_layout`; `bar` / widget / placement roots and dynamic passes live in `config_export_rust.rs`. Runtime `export_effective_toml` is Rust-only.
4. Port **bar/widget placement** emitters from `config_export.cpp` anonymous namespace.
5. **Done:** frozen fixtures, in-process runtime export, `helix-rust-config-export` (`cargo build -p helix-shell-core --bin helix-rust-config-export`) for `tools/gen_config_export_*`; the C++ `helix-config-export` target and `config_export_main.cpp` are gone; no C++ export binary in CI or fixture scripts.

`just build` compiles `helix-rust-config-export` with the rest of the workspace.
