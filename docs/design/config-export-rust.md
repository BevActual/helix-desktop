# Rust `config_export` port

Theme templates read `config.*` from `config_export::serialize` output (C++ today via `helix-config-export`).

## Parity gates

| Gate | Command / artifact |
|------|-------------------|
| Minimal fixture byte match | `cargo test -p helix-shell-core --test config_export_golden` |
| Regenerate golden | `tools/gen_config_export_golden.sh` |
| Schema read/write inverses | `meson test config_schema_roundtrip` (`tests/config_schema_roundtrip_test.cpp`) |

## C++ sources of truth

- Merge + parse: `ConfigService::buildEffectiveConfigFromSources` (`src/config/config_service.cpp`)
- Serialize: `config_export::serialize` (`src/config/config_export.cpp`)
- Section list: `helix::config::schema::sections()` (`src/config/schema/config_sections.cpp`)
- Extra root tables: `bar`, `widget`, `desktop_widgets`, `lockscreen_widgets`

## Rust implementation plan

1. **`helix-shell-core::desktop::config_export::rust_export_toml`** — return `None` until complete; wire ahead of C++ in `export_effective_toml`.
2. Port **merge + `normalizeLegacyConfig` + `parseConfigTable`** into Rust (or generate from schema), starting with sections templates touch most (`theme`, `shell`, `bar`).
3. Port **`writeTable` per section** in the same order as `config_sections.cpp`.
4. Port **bar/widget placement** emitters from `config_export.cpp` anonymous namespace.
5. When `config_export_golden` passes **without** `HELIX_CONFIG_EXPORT_BIN`, remove `helix-config-export` from Meson/CI.

Until step 5, `just build debug` (or install layout) must ship `helix-config-export` next to `helix-desktop` or under `build-debug/`.
