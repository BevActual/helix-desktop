//! `shell` section: composed typed subtrees + `merge_export_subtree` for large tables.

use toml::Value as Toml;

use super::config_export_defaults::write_shell_defaults;
use super::config_export_rust::{coerce_patch_value, merge_export_subtree, merged_overlay_for_parsed_section};
use super::config_export_serialize::f32_bits;
use super::config_export_typed_scalar::overlay_known_leaves;

const OVERLAY_SUBTREE_KEYS: &[&str] = &[
    "animation",
    "greeter_sync",
    "mpris",
    "privacy",
    "screen_corners",
    "screen_record",
    "screenshot",
    "shadow",
    "window_switcher",
];

fn shell_panel_export(merged_shell: Option<&Toml>) -> Toml {
    let defaults = write_shell_defaults();
    let panel_defaults = defaults.get("panel").expect("shell.panel defaults");
    let merged_panel = merged_shell.and_then(|shell| shell.get("panel"));
    let mut out = match merged_panel {
        Some(panel) => overlay_known_leaves(panel_defaults, panel),
        None => panel_defaults.clone(),
    };
    if let Some(panel) = merged_panel.and_then(Toml::as_table) {
        if let Some(glass) = panel.get("glass") {
            let coerced = coerce_patch_value(
                &Toml::Float(0.0),
                glass,
            );
            out.as_table_mut()
                .expect("panel export table")
                .insert("glass".to_string(), coerced);
        }
    }
    out
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct ShellScreenRecordExport {
    audio: bool,
    directory: String,
    filename_pattern: String,
    replay_duration: i64,
    show_cursor: bool,
}

impl ShellScreenRecordExport {
    fn export_defaults() -> Self {
        Self::from_export_table(
            write_shell_defaults()
                .get("screen_record")
                .expect("shell.screen_record defaults"),
        )
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("screen_record table");
        Self {
            audio: table.get("audio").and_then(Toml::as_bool).unwrap_or(true),
            directory: table
                .get("directory")
                .and_then(Toml::as_str)
                .unwrap_or("")
                .to_string(),
            filename_pattern: table
                .get("filename_pattern")
                .and_then(Toml::as_str)
                .unwrap_or("")
                .to_string(),
            replay_duration: table
                .get("replay_duration")
                .and_then(Toml::as_integer)
                .unwrap_or(30),
            show_cursor: table
                .get("show_cursor")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
        }
    }

    fn from_merged_section(merged_shell: Option<&Toml>) -> Self {
        let defaults = write_shell_defaults();
        let default_sub = defaults.get("screen_record").expect("screen_record");
        let merged_sub = merged_shell.and_then(|shell| shell.get("screen_record"));
        let table = match merged_sub {
            Some(merged) => overlay_known_leaves(default_sub, merged),
            None => default_sub.clone(),
        };
        Self::from_export_table(&table)
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("audio".to_string(), Toml::Boolean(self.audio));
        table.insert("directory".to_string(), Toml::String(self.directory.clone()));
        table.insert(
            "filename_pattern".to_string(),
            Toml::String(self.filename_pattern.clone()),
        );
        table.insert("replay_duration".to_string(), Toml::Integer(self.replay_duration));
        table.insert("show_cursor".to_string(), Toml::Boolean(self.show_cursor));
        Toml::Table(table)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct ShellScreenshotExport {
    annotate: bool,
    close_on_copy: bool,
    close_on_save: bool,
    confirm_region: bool,
    copy_to_clipboard: bool,
    directory: String,
    filename_pattern: String,
    freeze_screen: bool,
    pipe_command: String,
    pipe_to_command: bool,
    remember_last_region: bool,
    save_to_file: bool,
    show_cursor: bool,
    skip_annotate_on_copy_save: bool,
}

impl ShellScreenshotExport {
    fn export_defaults() -> Self {
        Self::from_export_table(
            write_shell_defaults()
                .get("screenshot")
                .expect("shell.screenshot defaults"),
        )
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("screenshot table");
        Self {
            annotate: table.get("annotate").and_then(Toml::as_bool).unwrap_or(false),
            close_on_copy: table
                .get("close_on_copy")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            close_on_save: table
                .get("close_on_save")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            confirm_region: table
                .get("confirm_region")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            copy_to_clipboard: table
                .get("copy_to_clipboard")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            directory: table
                .get("directory")
                .and_then(Toml::as_str)
                .unwrap_or("")
                .to_string(),
            filename_pattern: table
                .get("filename_pattern")
                .and_then(Toml::as_str)
                .unwrap_or("")
                .to_string(),
            freeze_screen: table
                .get("freeze_screen")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            pipe_command: table
                .get("pipe_command")
                .and_then(Toml::as_str)
                .unwrap_or("")
                .to_string(),
            pipe_to_command: table
                .get("pipe_to_command")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            remember_last_region: table
                .get("remember_last_region")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            save_to_file: table
                .get("save_to_file")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            show_cursor: table
                .get("show_cursor")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            skip_annotate_on_copy_save: table
                .get("skip_annotate_on_copy_save")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
        }
    }

    fn from_merged_section(merged_shell: Option<&Toml>) -> Self {
        let defaults = write_shell_defaults();
        let default_sub = defaults.get("screenshot").expect("screenshot");
        let merged_sub = merged_shell.and_then(|shell| shell.get("screenshot"));
        let table = match merged_sub {
            Some(merged) => overlay_known_leaves(default_sub, merged),
            None => default_sub.clone(),
        };
        Self::from_export_table(&table)
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("annotate".to_string(), Toml::Boolean(self.annotate));
        table.insert("close_on_copy".to_string(), Toml::Boolean(self.close_on_copy));
        table.insert("close_on_save".to_string(), Toml::Boolean(self.close_on_save));
        table.insert("confirm_region".to_string(), Toml::Boolean(self.confirm_region));
        table.insert(
            "copy_to_clipboard".to_string(),
            Toml::Boolean(self.copy_to_clipboard),
        );
        table.insert("directory".to_string(), Toml::String(self.directory.clone()));
        table.insert(
            "filename_pattern".to_string(),
            Toml::String(self.filename_pattern.clone()),
        );
        table.insert("freeze_screen".to_string(), Toml::Boolean(self.freeze_screen));
        table.insert("pipe_command".to_string(), Toml::String(self.pipe_command.clone()));
        table.insert("pipe_to_command".to_string(), Toml::Boolean(self.pipe_to_command));
        table.insert(
            "remember_last_region".to_string(),
            Toml::Boolean(self.remember_last_region),
        );
        table.insert("save_to_file".to_string(), Toml::Boolean(self.save_to_file));
        table.insert("show_cursor".to_string(), Toml::Boolean(self.show_cursor));
        table.insert(
            "skip_annotate_on_copy_save".to_string(),
            Toml::Boolean(self.skip_annotate_on_copy_save),
        );
        Toml::Table(table)
    }
}

#[derive(Clone, Debug, PartialEq)]
struct ShellLauncherExport {
    app_grid: bool,
    auto_paste: String,
    categories: bool,
    compact: bool,
    fetch_exchange_rates: bool,
    pinned: Vec<String>,
    provider_prefix: String,
    show_app_actions: bool,
    show_app_origin_indicator: bool,
    show_icons: bool,
    sort_by_usage: bool,
    dmenu: Toml,
    panels: Toml,
}

impl ShellLauncherExport {
    fn export_defaults() -> Self {
        Self::from_export_table(
            write_shell_defaults()
                .get("launcher")
                .expect("shell.launcher defaults"),
        )
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("launcher table");
        Self {
            app_grid: table.get("app_grid").and_then(Toml::as_bool).unwrap_or(false),
            auto_paste: table
                .get("auto_paste")
                .and_then(Toml::as_str)
                .unwrap_or("auto")
                .to_string(),
            categories: table.get("categories").and_then(Toml::as_bool).unwrap_or(true),
            compact: table.get("compact").and_then(Toml::as_bool).unwrap_or(false),
            fetch_exchange_rates: table
                .get("fetch_exchange_rates")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            pinned: table
                .get("pinned")
                .and_then(Toml::as_array)
                .map(|items| {
                    items
                        .iter()
                        .filter_map(|item| item.as_str().map(str::to_string))
                        .collect()
                })
                .unwrap_or_default(),
            provider_prefix: table
                .get("provider_prefix")
                .and_then(Toml::as_str)
                .unwrap_or("/")
                .to_string(),
            show_app_actions: table
                .get("show_app_actions")
                .and_then(Toml::as_bool)
                .unwrap_or(false),
            show_app_origin_indicator: table
                .get("show_app_origin_indicator")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            show_icons: table.get("show_icons").and_then(Toml::as_bool).unwrap_or(true),
            sort_by_usage: table
                .get("sort_by_usage")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            dmenu: table.get("dmenu").cloned().unwrap_or_else(empty_table),
            panels: table.get("panels").cloned().unwrap_or_else(empty_table),
        }
    }

    fn from_merged(merged: &Toml, default: &Toml, shell_overlay: &Toml) -> Self {
        let defaults = write_shell_defaults();
        let launcher_defaults = defaults.get("launcher").expect("launcher");
        let patch = shell_overlay
            .get("launcher")
            .cloned()
            .unwrap_or_else(empty_table);
        let empty = empty_table();
        let mut patch_scalars = patch.as_table().cloned().unwrap_or_default();
        patch_scalars.remove("dmenu");
        patch_scalars.remove("panels");
        let mut table = overlay_known_leaves(launcher_defaults, &Toml::Table(patch_scalars));
        let out_table = table.as_table_mut().expect("launcher export table");
        let def_dmenu = launcher_defaults.get("dmenu").unwrap_or(&empty);
        let patch_dmenu = patch.get("dmenu").unwrap_or(&empty);
        out_table.insert(
            "dmenu".to_string(),
            merge_export_subtree(def_dmenu, patch_dmenu, merged, default, "shell.launcher.dmenu"),
        );
        let def_panels = launcher_defaults.get("panels").unwrap_or(&empty);
        let patch_panels = patch.get("panels").unwrap_or(&empty);
        out_table.insert(
            "panels".to_string(),
            merge_export_subtree(
                def_panels,
                patch_panels,
                merged,
                default,
                "shell.launcher.panels",
            ),
        );
        Self::from_export_table(&table)
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("app_grid".to_string(), Toml::Boolean(self.app_grid));
        table.insert("auto_paste".to_string(), Toml::String(self.auto_paste.clone()));
        table.insert("categories".to_string(), Toml::Boolean(self.categories));
        table.insert("compact".to_string(), Toml::Boolean(self.compact));
        table.insert(
            "fetch_exchange_rates".to_string(),
            Toml::Boolean(self.fetch_exchange_rates),
        );
        table.insert(
            "pinned".to_string(),
            Toml::Array(
                self.pinned
                    .iter()
                    .map(|item| Toml::String(item.clone()))
                    .collect(),
            ),
        );
        table.insert(
            "provider_prefix".to_string(),
            Toml::String(self.provider_prefix.clone()),
        );
        table.insert("show_app_actions".to_string(), Toml::Boolean(self.show_app_actions));
        table.insert(
            "show_app_origin_indicator".to_string(),
            Toml::Boolean(self.show_app_origin_indicator),
        );
        table.insert("show_icons".to_string(), Toml::Boolean(self.show_icons));
        table.insert("sort_by_usage".to_string(), Toml::Boolean(self.sort_by_usage));
        table.insert("dmenu".to_string(), self.dmenu.clone());
        table.insert("panels".to_string(), self.panels.clone());
        Toml::Table(table)
    }
}

fn empty_table() -> Toml {
    Toml::Table(toml::map::Map::new())
}

#[derive(Clone, Debug, PartialEq)]
struct ShellSessionExport {
    grid: bool,
    grid_columns: i64,
    show_shortcuts: bool,
    power: Toml,
    actions: Toml,
}

impl ShellSessionExport {
    fn export_defaults() -> Self {
        Self::from_export_table(
            write_shell_defaults()
                .get("session")
                .expect("shell.session defaults"),
        )
    }

    fn from_export_table(table: &Toml) -> Self {
        let table = table.as_table().expect("session table");
        Self {
            grid: table.get("grid").and_then(Toml::as_bool).unwrap_or(false),
            grid_columns: table
                .get("grid_columns")
                .and_then(Toml::as_integer)
                .unwrap_or(3),
            show_shortcuts: table
                .get("show_shortcuts")
                .and_then(Toml::as_bool)
                .unwrap_or(true),
            power: table.get("power").cloned().unwrap_or_else(empty_table),
            actions: table.get("actions").cloned().unwrap_or(Toml::Array(vec![])),
        }
    }

    fn from_merged(merged: &Toml, default: &Toml, shell_overlay: &Toml) -> Self {
        let defaults = write_shell_defaults();
        let session_defaults = defaults.get("session").expect("session");
        let patch = shell_overlay
            .get("session")
            .cloned()
            .unwrap_or_else(empty_table);
        let empty = empty_table();
        let mut patch_scalars = patch.as_table().cloned().unwrap_or_default();
        patch_scalars.remove("power");
        patch_scalars.remove("actions");
        let mut table = overlay_known_leaves(session_defaults, &Toml::Table(patch_scalars));
        let out_table = table.as_table_mut().expect("session export table");
        let patch_table = patch.as_table();
        let def_power = session_defaults.get("power").unwrap_or(&empty);
        if patch_table.is_some_and(|table| table.contains_key("power")) {
            let patch_power = patch.get("power").expect("power in patch");
            out_table.insert(
                "power".to_string(),
                merge_export_subtree(
                    def_power,
                    patch_power,
                    merged,
                    default,
                    "shell.session.power",
                ),
            );
        }
        let empty_actions = Toml::Array(vec![]);
        let def_actions = session_defaults.get("actions").unwrap_or(&empty_actions);
        if patch_table.is_some_and(|table| table.contains_key("actions")) {
            let patch_actions = patch.get("actions").expect("actions in patch");
            out_table.insert(
                "actions".to_string(),
                merge_export_subtree(
                    def_actions,
                    patch_actions,
                    merged,
                    default,
                    "shell.session.actions",
                ),
            );
        }
        Self::from_export_table(&table)
    }

    fn write_export_table(&self) -> Toml {
        let mut table = toml::map::Map::new();
        table.insert("grid".to_string(), Toml::Boolean(self.grid));
        table.insert("grid_columns".to_string(), Toml::Integer(self.grid_columns));
        table.insert("show_shortcuts".to_string(), Toml::Boolean(self.show_shortcuts));
        table.insert("power".to_string(), self.power.clone());
        table.insert("actions".to_string(), self.actions.clone());
        Toml::Table(table)
    }
}

#[derive(Clone, Debug, PartialEq)]
struct ShellKeyboardLayoutExport {
    table: Toml,
}

impl ShellKeyboardLayoutExport {
    fn export_defaults() -> Self {
        Self {
            table: write_shell_defaults()
                .get("keyboard_layout")
                .expect("shell.keyboard_layout defaults")
                .clone(),
        }
    }

    fn from_merged(merged: &Toml, default: &Toml, shell_overlay: &Toml) -> Self {
        let defaults = write_shell_defaults();
        let default_sub = defaults.get("keyboard_layout").expect("keyboard_layout");
        let patch = shell_overlay
            .get("keyboard_layout")
            .cloned()
            .unwrap_or_else(empty_table);
        Self {
            table: merge_export_subtree(
                default_sub,
                &patch,
                merged,
                default,
                "shell.keyboard_layout",
            ),
        }
    }

    fn write_export_table(&self) -> Toml {
        self.table.clone()
    }
}

fn overlay_subtree(defaults: &Toml, merged_shell: Option<&Toml>, key: &str) -> Toml {
    let default_sub = defaults.get(key).expect("shell subtree default");
    let merged_sub = merged_shell.and_then(|shell| shell.get(key));
    match merged_sub {
        Some(merged) => overlay_known_leaves(default_sub, merged),
        None => default_sub.clone(),
    }
}

fn overlay_shell_root_scalars(
    out: &mut toml::map::Map<String, Toml>,
    merged_shell: Option<&Toml>,
    defaults: &toml::map::Map<String, Toml>,
) {
    let merged_table = merged_shell.and_then(Toml::as_table);
    for (key, default_value) in defaults {
        if default_value.is_table() {
            continue;
        }
        let merged_value = match merged_table.and_then(|table| table.get(key)) {
            Some(value) => value,
            None => continue,
        };
        out.insert(key.clone(), coerce_patch_value(default_value, merged_value));
    }
}

pub(crate) fn export_shell_section_from_merged(merged: &Toml, default: &Toml) -> Toml {
    let defaults = write_shell_defaults();
    let default_table = defaults.as_table().expect("shell defaults table");
    let merged_shell = merged.get("shell");
    let overlay = merged_shell
        .map(|section| merged_overlay_for_parsed_section("shell", section))
        .unwrap_or_else(|| Toml::Table(toml::map::Map::new()));

    let mut out = default_table.clone();
    overlay_shell_root_scalars(&mut out, merged_shell, default_table);

    out.insert("panel".to_string(), shell_panel_export(merged_shell));

    for key in OVERLAY_SUBTREE_KEYS {
        let value = match *key {
            "screenshot" => ShellScreenshotExport::from_merged_section(merged_shell).write_export_table(),
            "screen_record" => {
                ShellScreenRecordExport::from_merged_section(merged_shell).write_export_table()
            }
            _ => overlay_subtree(&defaults, merged_shell, key),
        };
        out.insert((*key).to_string(), value);
    }

    out.insert(
        "launcher".to_string(),
        ShellLauncherExport::from_merged(merged, default, &overlay).write_export_table(),
    );
    out.insert(
        "session".to_string(),
        ShellSessionExport::from_merged(merged, default, &overlay).write_export_table(),
    );
    out.insert(
        "keyboard_layout".to_string(),
        ShellKeyboardLayoutExport::from_merged(merged, default, &overlay).write_export_table(),
    );

    Toml::Table(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use super::super::config_export_rust::export_filtered_schema_section;
    use super::super::config_export_serialize::serialize_default_config;

    #[test]
    fn session_and_keyboard_layout_match_full_merge() {
        let default = serialize_default_config();
        let shell_defaults = write_shell_defaults();
        assert_eq!(
            ShellSessionExport::export_defaults().write_export_table(),
            shell_defaults.get("session").expect("session").clone(),
        );
        assert_eq!(
            ShellKeyboardLayoutExport::export_defaults().write_export_table(),
            shell_defaults
                .get("keyboard_layout")
                .expect("keyboard_layout")
                .clone(),
        );
        let merged: Toml = toml::from_str(
            r#"
[shell.session]
grid = true
grid_columns = 4

[[shell.session.actions]]
action = "lock"
enabled = false

[shell.session.power]
lid_close = "suspend"

[shell.keyboard_layout]
layout = "us"
"#,
        )
        .unwrap();
        let merged_shell = merged.get("shell").expect("shell");
        let shell_overlay = merged_overlay_for_parsed_section("shell", merged_shell);
        for (key, typed) in [
            (
                "session",
                ShellSessionExport::from_merged(&merged, &default, &shell_overlay)
                    .write_export_table(),
            ),
            (
                "keyboard_layout",
                ShellKeyboardLayoutExport::from_merged(&merged, &default, &shell_overlay)
                    .write_export_table(),
            ),
        ] {
            let default_sub = shell_defaults.get(key).unwrap();
            let patch = shell_overlay.get(key).unwrap();
            let via_merge = merge_export_subtree(
                default_sub,
                patch,
                &merged,
                &default,
                &format!("shell.{key}"),
            );
            assert_eq!(typed, via_merge, "subtree {key}");
        }
    }

    #[test]
    fn launcher_export_matches_defaults_and_full_merge() {
        let default = serialize_default_config();
        let shell_defaults = write_shell_defaults();
        assert_eq!(
            ShellLauncherExport::export_defaults().write_export_table(),
            shell_defaults.get("launcher").expect("launcher").clone(),
        );
        let merged: Toml = toml::from_str(
            r#"
[shell.launcher]
compact = true
pinned = ["firefox.desktop"]

[shell.launcher.dmenu]
width = 400
"#,
        )
        .unwrap();
        let merged_shell = merged.get("shell");
        let shell_overlay = merged_shell
            .map(|section| merged_overlay_for_parsed_section("shell", section))
            .expect("shell overlay");
        let launcher_defaults = shell_defaults.get("launcher").unwrap();
        let patch = shell_overlay.get("launcher").unwrap();
        let via_merge = merge_export_subtree(
            launcher_defaults,
            patch,
            &merged,
            &default,
            "shell.launcher",
        );
        let via_typed = ShellLauncherExport::from_merged(&merged, &default, &shell_overlay)
            .write_export_table();
        assert_eq!(via_typed, via_merge);
    }

    #[test]
    fn screenshot_and_screen_record_exports_match_defaults() {
        let shell_defaults = write_shell_defaults();
        assert_eq!(
            ShellScreenshotExport::export_defaults().write_export_table(),
            shell_defaults.get("screenshot").expect("screenshot").clone(),
        );
        assert_eq!(
            ShellScreenRecordExport::export_defaults().write_export_table(),
            shell_defaults
                .get("screen_record")
                .expect("screen_record")
                .clone(),
        );
    }

    #[test]
    fn composed_shell_matches_filtered_merge() {
        let default = serialize_default_config();
        let merged: Toml = toml::from_str(
            r#"
[shell]
polkit_agent = true
corner_radius_scale = 1.25

[shell.panel]
glass = 0.25
transparency_mode = "solid"

[shell.animation]
speed = 0.5

[shell.launcher]
compact = true

[shell.launcher.providers.calculator]
prefix = "calc"
"#,
        )
        .unwrap();
        let filtered =
            export_filtered_schema_section("shell", &merged, &default, write_shell_defaults);
        let composed = export_shell_section_from_merged(&merged, &default);
        assert_eq!(composed, filtered);
    }

    fn assert_subtree_overlay_matches_merge(key: &str, merged: &Toml, default: &Toml) {
        let defaults = write_shell_defaults();
        let default_sub = defaults.get(key).expect("shell subtree");
        let merged_shell = merged.get("shell");
        let overlay = merged_shell
            .map(|section| merged_overlay_for_parsed_section("shell", section))
            .unwrap_or_else(|| Toml::Table(toml::map::Map::new()));
        let patch = overlay
            .get(key)
            .cloned()
            .unwrap_or_else(|| Toml::Table(toml::map::Map::new()));
        let via_merge = merge_export_subtree(
            default_sub,
            &patch,
            merged,
            default,
            &format!("shell.{key}"),
        );
        let via_overlay = overlay_subtree(&defaults, merged_shell, key);
        assert_eq!(via_overlay, via_merge, "subtree {key}");
    }

    #[test]
    fn overlay_subtrees_match_merge_for_screenshot_and_screen_record() {
        let default = serialize_default_config();
        let merged: Toml = toml::from_str(
            r#"
[shell.screenshot]
annotate = true
directory = "/tmp/shots"
close_on_save = false

[shell.screen_record]
replay_duration = 60
audio = false
"#,
        )
        .unwrap();
        assert_subtree_overlay_matches_merge("screenshot", &merged, &default);
        assert_subtree_overlay_matches_merge("screen_record", &merged, &default);
    }

    #[test]
    fn panel_glass_uses_f32_bits() {
        let merged: Toml = toml::from_str(
            r#"
[shell.panel]
glass = 0.25000000000000006
"#,
        )
        .unwrap();
        let panel = shell_panel_export(merged.get("shell"));
        assert_eq!(
            panel.get("glass").and_then(|v| v.as_float()).map(f32_bits),
            Some(0.25)
        );
    }
}
