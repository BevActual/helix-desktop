use crate::contract::helpers::assert_setting_number;
use crate::harness::{json_array_value, sample_notification_rule, string_list, Session};
use helix_desktop_api::Desktop;
use serde_json::json;

pub async fn run(session: &Session, desktop: &Desktop) {
    let initial_mode = desktop
        .setting("theme.mode")
        .await
        .expect("theme.mode")
        .get("value")
        .cloned()
        .expect("theme.mode value");

    desktop
        .update_setting("theme.mode", &json!("light"))
        .await
        .expect("theme.mode light");
    assert!(desktop
        .update_setting("theme.mode", &json!("invalid"))
        .await
        .is_err());
    assert!(desktop
        .update_setting("dock.enabled", &json!(true))
        .await
        .is_err());
    desktop
        .reset_setting("theme.mode")
        .await
        .expect("reset theme");
    assert_eq!(
        desktop.setting("theme.mode").await.unwrap().get("value"),
        Some(&initial_mode)
    );

    assert!(desktop
        .update_setting("wallpaper.enabled", &json!(false))
        .await
        .is_err());
    assert!(desktop
        .update_setting("dock.launcher_position", &json!("start"))
        .await
        .is_err());
    assert!(desktop
        .update_setting("control_center.width", &json!(900))
        .await
        .is_err());

    desktop.register_desktop().await.expect("RegisterDesktop");
    desktop
        .register_capabilities(&json!({"gamma-control": true}))
        .await
        .expect("gamma");

    assert_eq!(
        desktop
            .setting("nightlight.enabled")
            .await
            .unwrap()
            .get("page")
            .and_then(|v| v.as_str()),
        Some("displays")
    );
    let original_day = desktop
        .setting("nightlight.temperature_day")
        .await
        .unwrap()
        .get("value")
        .cloned()
        .unwrap();
    let original_night = desktop
        .setting("nightlight.temperature_night")
        .await
        .unwrap()
        .get("value")
        .cloned()
        .unwrap();
    desktop
        .update_setting("nightlight.temperature_night", &json!(9000))
        .await
        .expect("night temp");
    assert_eq!(
        desktop
            .setting("nightlight.temperature_night")
            .await
            .unwrap()
            .get("value"),
        Some(&json!(9000))
    );
    assert!(
        desktop
            .setting("nightlight.temperature_day")
            .await
            .unwrap()
            .get("value")
            .and_then(|v| v.as_i64())
            .unwrap_or(0)
            >= 9100
    );
    desktop
        .update_setting("nightlight.temperature_day", &json!(2000))
        .await
        .expect("day temp");
    assert_eq!(
        desktop
            .setting("nightlight.temperature_day")
            .await
            .unwrap()
            .get("value"),
        Some(&json!(2000))
    );
    assert!(
        desktop
            .setting("nightlight.temperature_night")
            .await
            .unwrap()
            .get("value")
            .and_then(|v| v.as_i64())
            .unwrap_or(0)
            <= 1900
    );
    assert!(desktop
        .update_setting("nightlight.temperature_day", &json!(1000))
        .await
        .is_err());
    desktop
        .reset_setting("nightlight.temperature_night")
        .await
        .expect("reset night");
    assert_eq!(
        desktop
            .setting("nightlight.temperature_day")
            .await
            .unwrap()
            .get("value"),
        Some(&original_day)
    );
    assert_eq!(
        desktop
            .setting("nightlight.temperature_night")
            .await
            .unwrap()
            .get("value"),
        Some(&original_night)
    );
    assert!(desktop
        .register_capabilities(&json!({"unknown": true}))
        .await
        .is_err());
    desktop
        .register_capabilities(&json!({"gamma-control": false}))
        .await
        .expect("gamma off");
    assert!(desktop
        .update_setting("nightlight.enabled", &json!(true))
        .await
        .is_err());
    desktop
        .register_capabilities(&json!({"gamma-control": true}))
        .await
        .expect("gamma on again");

    let favorites = desktop.setting("dock.pinned").await.expect("dock.pinned");
    let dock_baseline = string_list(&favorites);
    desktop
        .update_application_list(
            "dock.pinned",
            &dock_baseline,
            &["test-first".into(), "test-second".into()],
        )
        .await
        .expect("dock pins");
    session.wait_for_settings_file().await;
    assert!(desktop
        .update_application_list("dock.pinned", &dock_baseline, &["stale".into()])
        .await
        .is_err());
    let dock_current = string_list(&desktop.setting("dock.pinned").await.unwrap());
    assert!(desktop
        .update_application_list(
            "dock.pinned",
            &dock_current,
            &["duplicate".into(), "duplicate".into()],
        )
        .await
        .is_err());
    desktop
        .update_application_list(
            "dock.pinned",
            &dock_current,
            &["test-second".into(), "test-first".into()],
        )
        .await
        .expect("reorder pins");

    let widgets = desktop
        .setting("bar:default:center")
        .await
        .expect("bar widgets");
    let widget_baseline = string_list(&widgets);
    desktop
        .update_ordered_list(
            "bar:default:center",
            &widget_baseline,
            &["output_volume".into(), "clock".into()],
        )
        .await
        .expect("bar widgets");
    assert_eq!(
        string_list(&desktop.setting("bar:default:center").await.unwrap()),
        vec!["output_volume".to_string(), "clock".to_string()]
    );
    assert!(desktop
        .update_ordered_list("bar:default:center", &widget_baseline, &["clock".into()])
        .await
        .is_err());
    let widgets_now = string_list(&desktop.setting("bar:default:center").await.unwrap());
    assert!(desktop
        .update_ordered_list(
            "bar:default:center",
            &widgets_now,
            &["missing-widget".into()],
        )
        .await
        .is_err());
    assert!(desktop
        .update_ordered_list(
            "bar:default:center",
            &widgets_now,
            &["clock".into(), "clock".into()],
        )
        .await
        .is_err());

    desktop
        .update_setting("dock.enabled", &json!(true))
        .await
        .expect("enable dock");
    assert!(desktop
        .commit_overrides("", "[dock]\nenabled=false\n")
        .await
        .is_err());

    let copy_keys = desktop
        .setting("keybinds.copy")
        .await
        .expect("keybinds.copy");
    let key_baseline = json_array_value(&copy_keys);
    let edited = json!(["Alt+F9", "Ctrl+F8"]);
    desktop
        .update_ordered_list_json("keybinds.copy", &key_baseline, &edited)
        .await
        .expect("edit copy");
    assert!(desktop
        .update_ordered_list_json("keybinds.copy", &key_baseline, &json!(["Ctrl+c"]))
        .await
        .is_err());
    for invalid in [
        json!([]),
        json!(["not-a-key"]),
        json!(["Super+c"]),
        json!(["Ctrl+c", "Ctrl+c"]),
        json!([42]),
    ] {
        assert!(desktop
            .update_ordered_list_json("keybinds.copy", &edited, &invalid)
            .await
            .is_err());
    }
    assert!(desktop
        .update_setting("keybinds.copy", &edited)
        .await
        .is_err());
    // Settings is its own application, not the registered shell: it may edit a list as it may change a scalar setting.
    let other = Desktop::session().await.expect("a second client");
    let key_now = json_array_value(&other.setting("keybinds.copy").await.expect("keybinds.copy"));
    other
        .update_ordered_list_json("keybinds.copy", &key_now, &json!(["Alt+F7"]))
        .await
        .expect("a client other than the shell edits a list");
    assert_eq!(
        json_array_value(&desktop.setting("keybinds.copy").await.unwrap()),
        json!(["Alt+F7"]),
        "the shell sees the other client's edit"
    );
    // The shell-only methods stay shell-only.
    assert!(other
        .commit_overrides("", "[dock]\nenabled=false\n")
        .await
        .is_err());
    assert!(other
        .register_capabilities(&json!({"gamma-control": true}))
        .await
        .is_err());
    desktop
        .reset_setting("keybinds.copy")
        .await
        .expect("reset keys");

    let rule = sample_notification_rule();
    let rules_row = desktop.setting("notification.filter").await.expect("rules");
    let pristine_rules = json_array_value(&rules_row);
    desktop
        .update_ordered_list_json(
            "notification.filter",
            &pristine_rules,
            &json!([rule.clone()]),
        )
        .await
        .expect("add rule");
    assert!(desktop
        .update_ordered_list_json("notification.filter", &pristine_rules, &json!([]))
        .await
        .is_err());
    let rules_baseline = json_array_value(&desktop.setting("notification.filter").await.unwrap());
    let mut invalid = rule.clone();
    invalid["match_content"] = json!("[");
    assert!(desktop
        .update_ordered_list_json("notification.filter", &rules_baseline, &json!([invalid]))
        .await
        .is_err());
    invalid = rule.clone();
    invalid["play_sound"] = json!("true");
    assert!(desktop
        .update_ordered_list_json("notification.filter", &rules_baseline, &json!([invalid]))
        .await
        .is_err());
    invalid = rule.clone();
    invalid["allowed_urgencies"] = json!(["emergency"]);
    assert!(desktop
        .update_ordered_list_json("notification.filter", &rules_baseline, &json!([invalid]))
        .await
        .is_err());
    invalid = rule.clone();
    invalid["override_duration"] = json!(2_147_483_648_i64);
    assert!(desktop
        .update_ordered_list_json("notification.filter", &rules_baseline, &json!([invalid]))
        .await
        .is_err());
    invalid = rule.clone();
    invalid["match"] = json!("");
    invalid["match_content"] = json!("");
    assert!(desktop
        .update_ordered_list_json("notification.filter", &rules_baseline, &json!([invalid]))
        .await
        .is_err());
    assert!(desktop
        .update_ordered_list_json(
            "notification.filter",
            &rules_baseline,
            &json!([rule.clone(), rule.clone()]),
        )
        .await
        .is_err());

    let monitors = desktop
        .setting("notification.monitors")
        .await
        .expect("monitors");
    let monitor_baseline = json_array_value(&monitors);
    desktop
        .update_ordered_list_json(
            "notification.monitors",
            &monitor_baseline,
            &json!(["DP-1", "eDP-1"]),
        )
        .await
        .expect("monitors");
    assert!(desktop
        .update_ordered_list_json(
            "notification.monitors",
            &monitor_baseline,
            &json!(["stale-display"]),
        )
        .await
        .is_err());
    let monitors_now = json_array_value(&desktop.setting("notification.monitors").await.unwrap());
    assert!(desktop
        .update_ordered_list_json(
            "notification.monitors",
            &monitors_now,
            &json!(["DP-1", "DP-1"]),
        )
        .await
        .is_err());
    assert!(desktop
        .update_ordered_list_json("notification.monitors", &monitors_now, &json!([42]))
        .await
        .is_err());

    let transitions = desktop
        .setting("wallpaper.transition")
        .await
        .expect("wallpaper.transition");
    assert_eq!(
        transitions.get("type").and_then(|v| v.as_str()),
        Some("transition-list")
    );
    let baseline = json_array_value(&transitions);
    assert!(desktop
        .update_setting("wallpaper.transition", &baseline)
        .await
        .is_err());
    let selected = json!(["fade", "wipe"]);
    desktop
        .update_ordered_list_json("wallpaper.transition", &baseline, &selected)
        .await
        .expect("set transitions");
    assert_eq!(
        json_array_value(&desktop.setting("wallpaper.transition").await.unwrap()),
        selected
    );
    desktop
        .reset_setting("wallpaper.transition")
        .await
        .expect("reset transitions");

    assert_eq!(
        desktop
            .setting("idle:lock:timeout")
            .await
            .unwrap()
            .get("page")
            .and_then(|v| v.as_str()),
        Some("power")
    );
    desktop
        .update_setting("idle:lock:timeout", &json!(420))
        .await
        .expect("idle timeout");
    assert_setting_number(&desktop.setting("idle:lock:timeout").await.unwrap(), 420.0);
    assert!(desktop
        .update_setting("idle:lock:timeout", &json!(-1))
        .await
        .is_err());
    assert_eq!(
        desktop
            .setting("idle:lock:action")
            .await
            .unwrap()
            .get("value"),
        Some(&json!("lock"))
    );
    desktop
        .update_setting("battery.warning_threshold", &json!(25))
        .await
        .expect("battery warn");
    assert!(desktop
        .update_setting("battery.warning_threshold", &json!(101))
        .await
        .is_err());
}

pub async fn run_persistence(session: &mut Session, desktop: &Desktop) {
    desktop
        .update_setting("dock.launcher_position", &json!("start"))
        .await
        .expect("launcher position");
    desktop
        .update_setting("wallpaper.fill_mode", &json!("fit"))
        .await
        .expect("fill mode");
    desktop
        .update_setting("bar:default:thickness", &json!(52))
        .await
        .expect("bar thickness");

    std::fs::write(session.config_toml(), "[theme]\nmode = \"light\"\n")
        .expect("rewrite config.toml");
    super::helpers::wait_theme_mode(desktop, "light").await;

    session.restart_service(desktop).await;
    assert_eq!(
        desktop.setting("theme.mode").await.unwrap().get("value"),
        Some(&json!("light"))
    );
    desktop.register_desktop().await.expect("re-register");
    assert_eq!(
        desktop.setting("dock.enabled").await.unwrap().get("value"),
        Some(&json!(true))
    );
    assert_eq!(
        desktop
            .setting("wallpaper.fill_mode")
            .await
            .unwrap()
            .get("value"),
        Some(&json!("fit"))
    );
    assert_eq!(
        desktop
            .setting("bar:default:thickness")
            .await
            .unwrap()
            .get("value"),
        Some(&json!(52))
    );
    assert_eq!(
        desktop
            .setting("dock.launcher_position")
            .await
            .unwrap()
            .get("value"),
        Some(&json!("start"))
    );
    assert_eq!(
        string_list(&desktop.setting("dock.pinned").await.unwrap()),
        vec!["test-second".to_string(), "test-first".to_string()]
    );
}
