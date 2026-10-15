//! Incremental port of C++ `config_export::serialize` (`src/config/config_export.cpp`).
//!
//! Milestone A (today): golden-default patch in `config_export_rust.rs`, then
//! [`finalize_serialized_export`] to match C++ ordering and omission rules.
//! Milestone B: `writeTable` per `config_sections.cpp` without an embedded golden.
//! Milestone C: `parseConfigTable` on merged user config before serialize.

use toml::Value as Toml;

fn sort_table_keys(table: &mut toml::map::Map<String, Toml>) {
    let mut entries: Vec<(String, Toml)> = table
        .iter()
        .map(|(key, value)| (key.clone(), value.clone()))
        .collect();
    entries.sort_by(|a, b| a.0.cmp(&b.0));
    table.clear();
    for (key, value) in entries {
        table.insert(key, value);
    }
}

fn sort_nested_table(root: &mut Toml, section: &str, child: &str) {
    let child_table = root
        .get_mut(section)
        .and_then(Toml::as_table_mut)
        .and_then(|section_table| section_table.get_mut(child))
        .and_then(Toml::as_table_mut);
    if let Some(table) = child_table {
        sort_table_keys(table);
    }
}

/// C++ `serialize` sorts `config.widgets` by name before `widgetConfigTable`.
fn sort_widget_section(root: &mut Toml) {
    if let Some(widgets) = root.get_mut("widget").and_then(Toml::as_table_mut) {
        sort_table_keys(widgets);
    }
}

/// Dynamic `calendar.account.<id>` tables are emitted in sorted id order.
fn sort_calendar_accounts(root: &mut Toml) {
    sort_nested_table(root, "calendar", "account");
}

/// `widgetsPlacementTable` walks `widget_order` but stores widgets in a sorted map.
fn sort_placement_widget_tables(root: &mut Toml) {
    for section in ["desktop_widgets", "lockscreen_widgets"] {
        sort_nested_table(root, section, "widget");
    }
}

/// C++ only writes `flip_x` / `flip_y` when true.
fn strip_false_placement_flip_flags(root: &mut Toml) {
    for section in ["desktop_widgets", "lockscreen_widgets"] {
        let widgets = root
            .get_mut(section)
            .and_then(|value| value.get_mut("widget"))
            .and_then(Toml::as_table_mut);
        let widgets = match widgets {
            Some(table) => table,
            None => continue,
        };
        let names: Vec<String> = widgets.keys().cloned().collect();
        for name in names {
            let table = match widgets.get_mut(&name).and_then(Toml::as_table_mut) {
                Some(table) => table,
                None => continue,
            };
            if table.get("flip_x") == Some(&Toml::Boolean(false)) {
                table.remove("flip_x");
            }
            if table.get("flip_y") == Some(&Toml::Boolean(false)) {
                table.remove("flip_y");
            }
        }
    }
}

/// Post-process export trees to match `config_export::serialize` shape.
pub fn finalize_serialized_export(root: &mut Toml) {
    sort_widget_section(root);
    sort_calendar_accounts(root);
    sort_placement_widget_tables(root);
    strip_false_placement_flip_flags(root);
}
