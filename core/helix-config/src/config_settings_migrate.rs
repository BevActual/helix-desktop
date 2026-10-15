//! `settings.toml` sidecar migrations (`helix::config::applyPendingConfigMigrations`).
//! Keep `CURRENT_CONFIG_VERSION` in sync with `config_migrations.cpp`.

use toml::map::Map;
use toml::Value as Toml;

pub const CONFIG_VERSION_KEY: &str = "config_version";
/// Last `toVersion` in `configMigrations()` (`config_migrations.cpp`).
pub const CURRENT_CONFIG_VERSION: i32 = 18;

const MAX_BAR_RADIUS: i64 = 500;
const BAR_RADIUS_KEYS: [&str; 5] = [
    "radius",
    "radius_top_left",
    "radius_top_right",
    "radius_bottom_left",
    "radius_bottom_right",
];

const RETIRED_CONTROL_CENTER_SERVICE_TABS: [&str; 4] = ["phone", "localsend", "syncthing", "cast"];

fn table_mut(root: &mut Toml) -> Option<&mut Map<String, Toml>> {
    root.as_table_mut()
}

fn ensure_table<'a>(parent: &'a mut Map<String, Toml>, key: &str) -> Option<&'a mut Map<String, Toml>> {
    if !parent.contains_key(key) {
        parent.insert(key.to_string(), Toml::Table(Map::new()));
    }
    parent.get_mut(key)?.as_table_mut()
}

fn bind_action(widget: &mut Toml, gesture: &str, action: &str) {
    let widget_table = widget.as_table_mut().unwrap();
    if !widget_table.contains_key("actions") {
        widget_table.insert("actions".to_string(), Toml::Table(Map::new()));
    }
    let actions = widget_table.get_mut("actions").unwrap().as_table_mut().unwrap();
    if !actions.contains_key(gesture) {
        actions.insert(gesture.to_string(), Toml::String(action.to_string()));
    }
}

fn migrate_negative_radii(table: &mut Map<String, Toml>) -> bool {
    let mut changed = false;
    for key in BAR_RADIUS_KEYS {
        let radius = table.get(key).and_then(Toml::as_integer);
        if radius.is_none_or(|r| r >= 0) {
            continue;
        }
        let r = radius.unwrap();
        let magnitude = if r <= -MAX_BAR_RADIUS {
            MAX_BAR_RADIUS
        } else {
            -r
        };
        table.insert(key.to_string(), Toml::Integer(magnitude));
        changed = true;
    }
    if changed {
        table.insert("concave_edge_corners".to_string(), Toml::Boolean(true));
    }
    changed
}

fn migrate_negative_bar_radii(root: &mut Toml) {
    let bars = match root.get_mut("bar").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    for (_, bar_node) in bars.iter_mut() {
        if let Some(bar) = bar_node.as_table_mut() {
            migrate_negative_radii(bar);
            if let Some(monitors) = bar.get_mut("monitor").and_then(Toml::as_table_mut) {
                for (_, monitor_node) in monitors.iter_mut() {
                    if let Some(monitor) = monitor_node.as_table_mut() {
                        migrate_negative_radii(monitor);
                    }
                }
            }
        }
    }
}

fn migrate_custom_schedule(root: &mut Toml) {
    let location = match root.get_mut("location").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    if location.contains_key("custom_schedule") {
        return;
    }
    let sunset = location
        .get("sunset")
        .and_then(Toml::as_str)
        .filter(|s| !s.is_empty());
    let sunrise = location
        .get("sunrise")
        .and_then(Toml::as_str)
        .filter(|s| !s.is_empty());
    if sunset.is_none() || sunrise.is_none() {
        return;
    }
    let auto_locate = location
        .get("auto_locate")
        .and_then(Toml::as_bool)
        .unwrap_or(false);
    let has_address = location
        .get("address")
        .and_then(Toml::as_str)
        .is_some_and(|s| !s.is_empty());
    let has_coordinates = location.get("latitude").is_some() && location.get("longitude").is_some();
    if auto_locate || has_address || has_coordinates {
        return;
    }
    location.insert("custom_schedule".to_string(), Toml::Boolean(true));
}

fn migrate_widget_actions(root: &mut Toml) {
    let shell = match root.get_mut("shell").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    if !shell.contains_key("middle_click_opens_widget_settings") {
        return;
    }
    let was_enabled = shell
        .get("middle_click_opens_widget_settings")
        .and_then(Toml::as_bool)
        .unwrap_or(true);
    shell.remove("middle_click_opens_widget_settings");
    if was_enabled {
        return;
    }
    if !root.is_table() {
        *root = Toml::Table(Map::new());
    }
    let root_table = root.as_table_mut().unwrap();
    if !root_table.contains_key("bar") {
        root_table.insert("bar".to_string(), Toml::Table(Map::new()));
    }
    let bars = root_table.get_mut("bar").unwrap().as_table_mut().unwrap();
    if bars.is_empty() {
        bars.insert("default".to_string(), Toml::Table(Map::new()));
    }
    for (_, bar_node) in bars.iter_mut() {
        let bar = match bar_node.as_table_mut() {
            Some(table) => table,
            None => continue,
        };
        if !bar.contains_key("actions") {
            bar.insert("actions".to_string(), Toml::Table(Map::new()));
        }
        let actions = bar.get_mut("actions").unwrap().as_table_mut().unwrap();
        if !actions.contains_key("middle") {
            actions.insert("middle".to_string(), Toml::String("none".to_string()));
        }
    }
}

fn widget_type(widget: &Toml, name: &str) -> String {
    widget
        .get("type")
        .and_then(Toml::as_str)
        .map(str::to_string)
        .unwrap_or_else(|| name.to_string())
}

fn migrate_widget_gesture_settings(root: &mut Toml) {
    const SCROLL_TYPES: [&str; 3] = ["workspaces", "taskbar", "power_profile"];
    let widgets = match root.get_mut("widget").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    for (widget_name, widget_node) in widgets.iter_mut() {
        let widget = match widget_node.as_table_mut() {
            Some(table) => table,
            None => continue,
        };
        let w = Toml::Table(widget.clone());
        let type_name = widget_type(&w, widget_name);
        if SCROLL_TYPES.contains(&type_name.as_str()) && widget.contains_key("enable_scroll") {
            let was_enabled = widget
                .get("enable_scroll")
                .and_then(Toml::as_bool)
                .unwrap_or(true);
            widget.remove("enable_scroll");
            if !was_enabled {
                let mut actions_host = Toml::Table(widget.clone());
                bind_action(&mut actions_host, "scroll_up", "none");
                bind_action(&mut actions_host, "scroll_down", "none");
                if let Some(actions) = actions_host.get("actions") {
                    widget.insert("actions".to_string(), actions.clone());
                }
            }
        }
        if type_name == "keyboard_layout" && widget.contains_key("cycle_command") {
            let command = widget
                .get("cycle_command")
                .and_then(Toml::as_str)
                .map(str::to_string)
                .unwrap_or_default();
            widget.remove("cycle_command");
            if !command.is_empty() {
                let mut actions_host = Toml::Table(widget.clone());
                bind_action(&mut actions_host, "left", &format!("exec {command}"));
                if let Some(actions) = actions_host.get("actions") {
                    widget.insert("actions".to_string(), actions.clone());
                }
            }
        }
    }
}

fn migrate_remaining_widget_gestures(root: &mut Toml) {
    const DEFAULT_SCROLL_STEP: i64 = 5;
    let widgets = match root.get_mut("widget").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    for (widget_name, widget_node) in widgets.iter_mut() {
        let widget = match widget_node.as_table_mut() {
            Some(table) => table,
            None => continue,
        };
        let w = Toml::Table(widget.clone());
        let type_name = widget_type(&w, widget_name);
        let scroll_type = type_name == "media" || type_name == "volume" || type_name == "brightness";

        if scroll_type && widget.contains_key("enable_scroll") {
            let was_enabled = widget
                .get("enable_scroll")
                .and_then(Toml::as_bool)
                .unwrap_or(true);
            widget.remove("enable_scroll");
            if !was_enabled {
                let mut actions_host = Toml::Table(widget.clone());
                bind_action(&mut actions_host, "scroll_up", "none");
                bind_action(&mut actions_host, "scroll_down", "none");
                if let Some(actions) = actions_host.get("actions") {
                    widget.insert("actions".to_string(), actions.clone());
                }
            }
        }

        if (type_name == "volume" || type_name == "brightness") && widget.contains_key("scroll_step") {
            let step = widget
                .get("scroll_step")
                .and_then(Toml::as_integer)
                .unwrap_or(DEFAULT_SCROLL_STEP);
            widget.remove("scroll_step");
            if step != DEFAULT_SCROLL_STEP {
                let suffix = format!(" {step}%");
                let (up_verb, down_verb) = if type_name == "volume" {
                    let microphone = widget
                        .get("device")
                        .and_then(Toml::as_str)
                        .unwrap_or("output")
                        == "input";
                    if microphone {
                        ("mic-volume-up", "mic-volume-down")
                    } else {
                        ("volume-up", "volume-down")
                    }
                } else {
                    ("brightness-up", "brightness-down")
                };
                let mut actions_host = Toml::Table(widget.clone());
                bind_action(&mut actions_host, "scroll_up", &format!("{up_verb}{suffix}"));
                bind_action(&mut actions_host, "scroll_down", &format!("{down_verb}{suffix}"));
                if let Some(actions) = actions_host.get("actions") {
                    widget.insert("actions".to_string(), actions.clone());
                }
            }
        }

        if type_name == "screenshot" && widget.contains_key("primary_click") {
            let primary = widget
                .get("primary_click")
                .and_then(Toml::as_str)
                .map(str::to_string)
                .unwrap_or_else(|| "region".to_string());
            widget.remove("primary_click");
            if primary == "fullscreen" {
                let mut actions_host = Toml::Table(widget.clone());
                bind_action(&mut actions_host, "left", "screenshot-fullscreen");
                if let Some(actions) = actions_host.get("actions") {
                    widget.insert("actions".to_string(), actions.clone());
                }
            }
        }
    }
}

fn migrate_custom_button_commands(root: &mut Toml) {
    const COMMAND_KEYS: [(&str, &str); 5] = [
        ("command", "left"),
        ("right_command", "right"),
        ("middle_command", "middle"),
        ("scroll_up_command", "scroll_up"),
        ("scroll_down_command", "scroll_down"),
    ];
    let widgets = match root.get_mut("widget").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    for (widget_name, widget_node) in widgets.iter_mut() {
        let widget = match widget_node.as_table_mut() {
            Some(table) => table,
            None => continue,
        };
        let w = Toml::Table(widget.clone());
        if widget_type(&w, widget_name) != "custom_button" {
            continue;
        }
        if widget.contains_key("enable_scroll") {
            let was_enabled = widget
                .get("enable_scroll")
                .and_then(Toml::as_bool)
                .unwrap_or(true);
            widget.remove("enable_scroll");
            if !was_enabled {
                let mut actions_host = Toml::Table(widget.clone());
                bind_action(&mut actions_host, "scroll_up", "none");
                bind_action(&mut actions_host, "scroll_down", "none");
                if let Some(actions) = actions_host.get("actions") {
                    widget.insert("actions".to_string(), actions.clone());
                }
            }
        }
        for (key, gesture) in COMMAND_KEYS {
            if !widget.contains_key(key) {
                continue;
            }
            let command = widget
                .get(key)
                .and_then(Toml::as_str)
                .map(str::to_string)
                .unwrap_or_default();
            widget.remove(key);
            if !command.is_empty() {
                let mut actions_host = Toml::Table(widget.clone());
                bind_action(&mut actions_host, gesture, &format!("exec {command}"));
                if let Some(actions) = actions_host.get("actions") {
                    widget.insert("actions".to_string(), actions.clone());
                }
            }
        }
    }
}

fn migrate_dead_zone_table(owner: &mut Map<String, Toml>) {
    const COMMAND_KEYS: [(&str, &str); 5] = [
        ("command", "left"),
        ("right_command", "right"),
        ("middle_command", "middle"),
        ("scroll_up_command", "scroll_up"),
        ("scroll_down_command", "scroll_down"),
    ];
    let dead_zone = match owner.get_mut("dead_zone").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    for (key, gesture) in COMMAND_KEYS {
        if !dead_zone.contains_key(key) {
            continue;
        }
        let command = dead_zone
            .get(key)
            .and_then(Toml::as_str)
            .map(str::to_string)
            .unwrap_or_default();
        dead_zone.remove(key);
        if !command.is_empty() {
            let mut host = Toml::Table(dead_zone.clone());
            bind_action(&mut host, gesture, &format!("exec {command}"));
            if let Some(actions) = host.get("actions") {
                dead_zone.insert("actions".to_string(), actions.clone());
            }
        }
    }
}

fn migrate_dead_zone_actions(root: &mut Toml) {
    let bars = match root.get_mut("bar").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    for (_, bar_node) in bars.iter_mut() {
        if let Some(bar) = bar_node.as_table_mut() {
            migrate_dead_zone_table(bar);
            if let Some(monitors) = bar.get_mut("monitor").and_then(Toml::as_table_mut) {
                for (_, monitor_node) in monitors.iter_mut() {
                    if let Some(monitor) = monitor_node.as_table_mut() {
                        migrate_dead_zone_table(monitor);
                    }
                }
            }
        }
    }
}

fn migrate_lockscreen_login_box_deprecated_settings(root: &mut Toml) {
    let section = match root.get_mut("lockscreen_widgets").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    let widgets = match section.get_mut("widget").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    for (widget_id, widget_node) in widgets.iter_mut() {
        let widget = match widget_node.as_table_mut() {
            Some(table) => table,
            None => continue,
        };
        let type_name = widget
            .get("type")
            .and_then(Toml::as_str)
            .unwrap_or("");
        if type_name != "login_box" && !widget_id.starts_with("lockscreen-login-box@") {
            continue;
        }
        if let Some(settings) = widget.get_mut("settings").and_then(Toml::as_table_mut) {
            settings.remove("show_password_hint");
        }
    }
}

fn migrate_sysmon_presentation_settings(root: &mut Toml) {
    let widgets = match root.get_mut("widget").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    for (widget_name, widget_node) in widgets.iter_mut() {
        let widget = match widget_node.as_table_mut() {
            Some(table) => table,
            None => continue,
        };
        let w = Toml::Table(widget.clone());
        if widget_type(&w, widget_name) != "sysmon" {
            continue;
        }

        if let Some(show_icon) = widget.get("show_icon").and_then(Toml::as_bool) {
            if !widget.contains_key("show_glyph") {
                widget.insert("show_glyph".to_string(), Toml::Boolean(show_icon));
            }
            widget.remove("show_icon");
        }

        let display = widget
            .get("display")
            .and_then(Toml::as_str)
            .map(str::to_string);

        if let Some(display) = display.as_deref() {
            if !widget.contains_key("visualization") {
                let visualization = if display == "text" { "none" } else { display };
                widget.insert(
                    "visualization".to_string(),
                    Toml::String(visualization.to_string()),
                );
            }
            widget.remove("display");
        }

        if let Some(show_label) = widget.get("show_label").and_then(Toml::as_bool) {
            if !widget.contains_key("show_value") {
                let show_value = if display.as_deref() == Some("text") {
                    true
                } else if display.as_deref() == Some("none") {
                    false
                } else {
                    show_label
                };
                widget.insert("show_value".to_string(), Toml::Boolean(show_value));
            }
            widget.remove("show_label");
        } else if let Some(display) = display.as_deref() {
            if !widget.contains_key("show_value") {
                widget.insert("show_value".to_string(), Toml::Boolean(display != "none"));
            }
        }
    }
}

fn migrate_workspaces_display_settings(root: &mut Toml) {
    let widgets = match root.get_mut("widget").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    for (widget_name, widget_node) in widgets.iter_mut() {
        let widget = match widget_node.as_table_mut() {
            Some(table) => table,
            None => continue,
        };
        let w = Toml::Table(widget.clone());
        if widget_type(&w, widget_name) != "workspaces" {
            continue;
        }
        let display = widget
            .get("display")
            .and_then(Toml::as_str)
            .map(str::to_string);
        let Some(display) = display else {
            continue;
        };
        if display == "none" {
            if !widget.contains_key("show_labels") {
                widget.insert("show_labels".to_string(), Toml::Boolean(false));
            }
        } else if display == "id" || display == "name" {
            if !widget.contains_key("label_source") {
                widget.insert("label_source".to_string(), Toml::String(display));
            }
        } else {
            continue;
        }
        widget.remove("display");
    }
}

fn migrate_keyboard_layout_show_glyph(root: &mut Toml) {
    let widgets = match root.get_mut("widget").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    for (widget_name, widget_node) in widgets.iter_mut() {
        let widget = match widget_node.as_table_mut() {
            Some(table) => table,
            None => continue,
        };
        let w = Toml::Table(widget.clone());
        if widget_type(&w, widget_name) != "keyboard_layout" {
            continue;
        }
        let show_icon = widget.get("show_icon").and_then(Toml::as_bool);
        if show_icon.is_none() {
            continue;
        }
        if !widget.contains_key("show_glyph") {
            widget.insert("show_glyph".to_string(), Toml::Boolean(show_icon.unwrap()));
        }
        widget.remove("show_icon");
    }
}

fn migrate_keyboard_layout_custom_labels(root: &mut Toml) {
    let widget_pairs: Vec<(String, Map<String, Toml>)> = root
        .get("widget")
        .and_then(Toml::as_table)
        .map(|widgets| {
            widgets
                .iter()
                .filter_map(|(name, node)| {
                    if widget_type(node, name) != "keyboard_layout" {
                        return None;
                    }
                    let custom = node
                        .get("custom_labels")
                        .and_then(Toml::as_table)
                        .cloned()?;
                    Some((name.clone(), custom))
                })
                .collect()
        })
        .unwrap_or_default();
    if widget_pairs.is_empty() {
        return;
    }
    let root_table = match table_mut(root) {
        Some(table) => table,
        None => return,
    };
    let shell = ensure_table(root_table, "shell");
    let keyboard_layout = shell.and_then(|shell| ensure_table(shell, "keyboard_layout"));
    let target_labels = keyboard_layout.and_then(|kl| ensure_table(kl, "custom_labels"));
    if let Some(target_labels) = target_labels {
        for (_, custom_labels) in &widget_pairs {
            for (layout_name, label_node) in custom_labels {
                let label = label_node.as_str();
                if label.is_none() || target_labels.contains_key(layout_name) {
                    continue;
                }
                target_labels.insert(layout_name.clone(), Toml::String(label.unwrap().to_string()));
            }
        }
    }
    if let Some(widgets) = root_table.get_mut("widget").and_then(Toml::as_table_mut) {
        for (widget_name, _) in widget_pairs {
            if let Some(widget) = widgets.get_mut(&widget_name).and_then(Toml::as_table_mut) {
                widget.remove("custom_labels");
            }
        }
    }
}

fn migrate_plugin_auto_update_mode(root: &mut Toml) {
    let plugins = match root.get_mut("plugins").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    let legacy = plugins.get("auto_update").and_then(Toml::as_bool);
    if legacy.is_none() {
        return;
    }
    let mode = if legacy.unwrap() { "all" } else { "none" };
    plugins.insert("auto_update".to_string(), Toml::String(mode.to_string()));
}

fn migrate_calendar_event_formats(root: &mut Toml) {
    let moves: Vec<(String, Toml)> = root
        .get("control_center")
        .and_then(Toml::as_table)
        .and_then(|cc| cc.get("calendar"))
        .and_then(Toml::as_table)
        .map(|calendar_tab| {
            ["event_date_format", "event_time_format"]
                .iter()
                .filter_map(|key| {
                    calendar_tab
                        .get(*key)
                        .map(|node| (key.to_string(), node.clone()))
                })
                .collect()
        })
        .unwrap_or_default();
    if moves.is_empty() {
        return;
    }
    let root_table = table_mut(root).unwrap();
    if !root_table.contains_key("calendar") {
        root_table.insert("calendar".to_string(), Toml::Table(Map::new()));
    } else if !root_table.get("calendar").is_some_and(Toml::is_table) {
        return;
    }
    let calendar = root_table.get_mut("calendar").unwrap().as_table_mut().unwrap();
    for (key, node) in moves {
        if !calendar.contains_key(&key) {
            calendar.insert(key, node);
        }
    }
    if let Some(calendar_tab) = root_table
        .get_mut("control_center")
        .and_then(Toml::as_table_mut)
        .and_then(|cc| cc.get_mut("calendar"))
        .and_then(Toml::as_table_mut)
    {
        for key in ["event_date_format", "event_time_format"] {
            calendar_tab.remove(key);
        }
    }
}

fn migrate_launch_apps_as_systemd_services(root: &mut Toml) {
    let shell = match root.get_mut("shell").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    let custom = shell
        .get("launch_apps_custom_command")
        .and_then(Toml::as_str)
        .filter(|s| !s.is_empty());
    if custom.is_some() {
        return;
    }
    let enabled = shell.get("launch_apps_as_systemd_services").and_then(Toml::as_bool);
    if !enabled.is_some_and(|v| !v) {
        return;
    }
    shell.insert(
        "launch_apps_as_systemd_services".to_string(),
        Toml::Boolean(true),
    );
}

fn migrate_calendar_timezones_to_clock(root: &mut Toml) {
    let old_node = root
        .get("control_center")
        .and_then(Toml::as_table)
        .and_then(|cc| cc.get("calendar"))
        .and_then(Toml::as_table)
        .and_then(|calendar| calendar.get("timezones"))
        .cloned();
    if old_node.is_none() {
        return;
    }
    let old_node = old_node.unwrap();
    let root_table = table_mut(root).unwrap();
    let control_center = match root_table.get_mut("control_center").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    let has_clock_table = control_center.get("clock").is_some_and(Toml::is_table);
    let clock_missing_or_table = !control_center.contains_key("clock") || has_clock_table;
    if !clock_missing_or_table {
        if let Some(calendar_tab) = control_center.get_mut("calendar").and_then(Toml::as_table_mut) {
            calendar_tab.remove("timezones");
        }
        return;
    }
    if !control_center.contains_key("clock") {
        control_center.insert("clock".to_string(), Toml::Table(Map::new()));
    }
    if let Some(clock_tab) = control_center.get_mut("clock").and_then(Toml::as_table_mut) {
        if !clock_tab.contains_key("timezones") {
            clock_tab.insert("timezones".to_string(), old_node);
        }
    }
    if let Some(calendar_tab) = control_center.get_mut("calendar").and_then(Toml::as_table_mut) {
        calendar_tab.remove("timezones");
    }
}

fn migrate_control_center_shortcut_labels(root: &mut Toml) {
    let section = match root.get_mut("control_center").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    section.remove("show_shortcut_labels");
}

fn is_retired_control_center_service_tab(id: &str) -> bool {
    RETIRED_CONTROL_CENTER_SERVICE_TABS.contains(&id)
}

fn migrate_retired_control_center_services(root: &mut Toml) {
    let root_table = match table_mut(root) {
        Some(table) => table,
        None => return,
    };
    root_table.remove("localsend");
    root_table.remove("phone");
    let control_center = match root_table.get_mut("control_center").and_then(Toml::as_table_mut) {
        Some(table) => table,
        None => return,
    };
    if let Some(shortcuts) = control_center
        .get_mut("shortcuts")
        .and_then(|node| node.as_array())
    {
        let filtered: Vec<Toml> = shortcuts
            .iter()
            .filter(|entry| {
                let drop = entry
                    .as_table()
                    .and_then(|t| t.get("type"))
                    .and_then(Toml::as_str)
                    .is_some_and(is_retired_control_center_service_tab);
                !drop
            })
            .cloned()
            .collect();
        if filtered.len() != shortcuts.len() {
            control_center.insert("shortcuts".to_string(), Toml::Array(filtered));
        }
    }
    if let Some(hidden_tabs) = control_center
        .get_mut("hidden_tabs")
        .and_then(|node| node.as_array())
    {
        let filtered: Vec<Toml> = hidden_tabs
            .iter()
            .filter(|entry| {
                !entry
                    .as_str()
                    .is_some_and(is_retired_control_center_service_tab)
            })
            .cloned()
            .collect();
        if filtered.len() != hidden_tabs.len() {
            control_center.insert("hidden_tabs".to_string(), Toml::Array(filtered));
        }
    }
}

type MigrateFn = fn(&mut Toml);

const MIGRATIONS: &[(i32, MigrateFn)] = &[
    (1, migrate_negative_bar_radii),
    (2, migrate_custom_schedule),
    (3, migrate_widget_actions),
    (4, migrate_widget_gesture_settings),
    (5, migrate_remaining_widget_gestures),
    (6, migrate_custom_button_commands),
    (7, migrate_dead_zone_actions),
    (8, migrate_lockscreen_login_box_deprecated_settings),
    (9, migrate_sysmon_presentation_settings),
    (10, migrate_keyboard_layout_show_glyph),
    (11, migrate_workspaces_display_settings),
    (12, migrate_keyboard_layout_custom_labels),
    (13, migrate_plugin_auto_update_mode),
    (14, migrate_calendar_event_formats),
    (15, migrate_launch_apps_as_systemd_services),
    (16, migrate_calendar_timezones_to_clock),
    (17, migrate_control_center_shortcut_labels),
    (18, migrate_retired_control_center_services),
];

fn stored_config_version(root: &Map<String, Toml>) -> Option<i32> {
    match root.get(CONFIG_VERSION_KEY) {
        None => Some(0),
        Some(Toml::Integer(v)) if *v >= 0 && *v <= i64::from(i32::MAX) => {
            let version = *v as i32;
            if version > CURRENT_CONFIG_VERSION {
                return None;
            }
            Some(version)
        }
        _ => None,
    }
}

fn apply_pending_sidecar_migrations(root: &mut Toml, stored_version: i32) -> i32 {
    let mut applied = stored_version;
    for (to_version, apply) in MIGRATIONS {
        if *to_version <= stored_version {
            continue;
        }
        apply(root);
        applied = *to_version;
    }
    applied
}

/// Same transforms as C++ `normalizeLegacyConfig` on merged config (excludes sidecar-only
/// `launch_apps_as_systemd_services` migration).
const LEGACY_NORMALIZE: &[MigrateFn] = &[
    migrate_negative_bar_radii,
    migrate_custom_schedule,
    migrate_widget_actions,
    migrate_widget_gesture_settings,
    migrate_remaining_widget_gestures,
    migrate_custom_button_commands,
    migrate_dead_zone_actions,
    migrate_lockscreen_login_box_deprecated_settings,
    migrate_sysmon_presentation_settings,
    migrate_keyboard_layout_show_glyph,
    migrate_workspaces_display_settings,
    migrate_keyboard_layout_custom_labels,
    migrate_plugin_auto_update_mode,
    migrate_calendar_event_formats,
    migrate_calendar_timezones_to_clock,
    migrate_control_center_shortcut_labels,
    migrate_retired_control_center_services,
];

/// C++ `normalizeLegacyConfig` (mutates merged export input; diagnostics are not collected).
pub fn normalize_legacy_config(root: &mut Toml) {
    for apply in LEGACY_NORMALIZE {
        apply(root);
    }
}

/// Apply C++ sidecar migrations to a parsed `settings.toml` root table.
pub fn prepare_settings_sidecar(sidecar: Toml) -> Toml {
    let Toml::Table(table) = sidecar else {
        return sidecar;
    };
    if table.is_empty() {
        return Toml::Table(table);
    }
    let stored = match stored_config_version(&table) {
        Some(version) => version,
        None => return Toml::Table(table),
    };
    let mut root = Toml::Table(table);
    let applied = apply_pending_sidecar_migrations(&mut root, stored);
    if applied != stored {
        root.as_table_mut()
            .expect("sidecar root")
            .insert(CONFIG_VERSION_KEY.to_string(), Toml::Integer(i64::from(applied)));
    }
    root
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn launch_apps_sidecar_migration_matches_cpp() {
        let input = r#"
config_version = 14
[shell]
launch_apps_as_systemd_services = false
"#;
        let migrated = prepare_settings_sidecar(input.parse().expect("parse"));
        let shell = migrated
            .get("shell")
            .and_then(Toml::as_table)
            .expect("shell");
        assert_eq!(
            shell.get("launch_apps_as_systemd_services"),
            Some(&Toml::Boolean(true))
        );
        assert_eq!(
            migrated
                .get(CONFIG_VERSION_KEY)
                .and_then(Toml::as_integer),
            Some(i64::from(CURRENT_CONFIG_VERSION))
        );
    }
}
