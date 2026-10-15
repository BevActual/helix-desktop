//! The Rust bootstrap catalog must match the C++ catalog for a pristine config
//! (the golden is frozen; the C++ catalog it came from was removed).

use helix_config::settings::{ConfigPaths, ConfigStore};
use serde_json::Value;
use std::collections::BTreeMap;

/// Rust defaults that intentionally differ from the (retired) C++ config; keep in sync with
/// `PRODUCT_DEFAULTS` in tools/gen-rust-settings-defaults.py.
const PRODUCT_DEFAULTS: &[&str] = &["shell.polkit_agent"];

fn rows(v: &[Value]) -> BTreeMap<String, Value> {
    v.iter()
        .filter_map(|r| Some((r.get("id")?.as_str()?.to_string(), r.clone())))
        .collect()
}

fn same_number(a: &Value, b: &Value) -> bool {
    match (a.as_f64(), b.as_f64()) {
        (Some(x), Some(y)) => (x - y).abs() <= 1e-6 * y.abs().max(1.0),
        _ => a == b,
    }
}

#[test]
fn pristine_catalog_matches_golden() {
    let golden: Vec<Value> =
        serde_json::from_str(include_str!("golden/settings_catalog_defaults.json")).unwrap();
    let want = rows(&golden);

    let dir = std::env::temp_dir().join(format!("helix-catalog-parity-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    // The `ai.*` mirror reads the inference routing file from the state dir.
    std::env::set_var("HOME", &dir);
    std::env::set_var("XDG_STATE_HOME", dir.join("xdg-state"));
    std::env::set_var("XDG_CONFIG_HOME", dir.join("xdg-config"));
    let store = ConfigStore::load(ConfigPaths {
        config_dir: dir.join("cfg"),
        settings_toml: dir.join("state/settings.toml"),
    });
    let got = rows(&store.full_catalog(true, &[]));

    let mut problems = Vec::new();
    for (id, w) in &want {
        let Some(g) = got.get(id) else {
            problems.push(format!("missing in Rust: {id}"));
            continue;
        };
        for field in ["type", "page", "group"] {
            if g.get(field) != w.get(field) {
                problems.push(format!("{id}.{field}: rust={:?} cpp={:?}", g.get(field), w.get(field)));
            }
        }
        // Needs the installed template assets; the C++ golden ran without them.
        if id == "theme.templates.builtin_ids" {
            continue;
        }
        // Option lists (widget names, palettes, tabs, ...) are part of the contract too.
        let options = |row: &Value| row.get("options").cloned().unwrap_or_else(|| Value::Array(vec![]));
        if options(g) != options(w) {
            problems.push(format!("{id}.options: rust={} cpp={}", options(g), options(w)));
        }
        if PRODUCT_DEFAULTS.contains(&id.as_str()) {
            continue;
        }
        let (gv, wv) = (g.get("value").cloned().unwrap_or(Value::Null), w.get("value").cloned().unwrap_or(Value::Null));
        if !same_number(&gv, &wv) {
            problems.push(format!("{id}.value: rust={gv} cpp={wv}"));
        }
    }
    for id in got.keys().filter(|id| !want.contains_key(*id)) {
        problems.push(format!("extra in Rust: {id}"));
    }
    let _ = std::fs::remove_dir_all(&dir);
    assert!(problems.is_empty(), "{} mismatches:\n{}", problems.len(), problems.join("\n"));
}
