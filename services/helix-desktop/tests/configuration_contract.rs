//! Rust-native slice of `tests/desktop_service_test.cpp`. One session service for the
//! whole suite (avoids dbus races between test functions). Run via:
//!
//!   tools/nested/helix-desktop-configuration-contract.sh

mod harness;

use harness::{
    boot, contract_enabled, json_array_value, sample_notification_rule, string_list, value_i64,
};
use helix_desktop_api::Domain;
use serde_json::json;

#[tokio::test]
async fn desktop_service_configuration_contract() {
    if !contract_enabled() {
        eprintln!(
            "skip configuration_contract (set HELIX_RUN_CONFIGURATION_CONTRACT=1; see tools/nested/helix-desktop-configuration-contract.sh)"
        );
        return;
    }

    let (session, desktop) = boot().await;

    let config = desktop.snapshot(Domain::Configuration).await.expect("config snap");
    assert_eq!(config.get("helix-desktop"), Some(&json!(false)));

    let appearance = desktop.snapshot(Domain::Appearance).await.expect("appearance");
    assert_eq!(appearance.get("wallpaper"), Some(&json!("")));
    let previews = appearance
        .get("palette-previews")
        .and_then(|v| v.as_array())
        .expect("palette-previews");
    assert!(previews.len() >= 10);
    for preview in previews {
        assert!(preview.get("source").and_then(|v| v.as_str()).is_some());
        assert!(preview.get("name").and_then(|v| v.as_str()).is_some());
        for mode in ["dark", "light"] {
            let variant = preview.get(mode).expect(mode);
            assert!(variant.get("surface").and_then(|v| v.as_u64()).is_some());
            let accents = variant.get("accents").and_then(|v| v.as_array()).expect("accents");
            assert!(!accents.is_empty());
        }
    }
    assert!(
        appearance.get("wallhaven-online").and_then(|v| v.as_bool()).is_some(),
        "wallhaven-online flag must be present"
    );

    let initial_mode = desktop
        .setting("theme.mode")
        .await
        .expect("theme.mode row")
        .get("value")
        .cloned()
        .expect("theme.mode value");

    desktop
        .update_setting("theme.mode", &json!("light"))
        .await
        .expect("update theme.mode");
    assert!(
        desktop
            .update_setting("theme.mode", &json!("invalid"))
            .await
            .is_err()
    );
    assert!(
        desktop
            .update_setting("dock.enabled", &json!(true))
            .await
            .is_err()
    );
    desktop
        .reset_setting("theme.mode")
        .await
        .expect("reset theme.mode");
    assert_eq!(
        desktop.setting("theme.mode").await.unwrap().get("value"),
        Some(&initial_mode)
    );

    desktop.register_desktop().await.expect("RegisterDesktop");
    desktop
        .register_capabilities(&json!({"gamma-control": true}))
        .await
        .expect("gamma-control");

    desktop
        .update_setting("nightlight.temperature_night", &json!(9000))
        .await
        .expect("raise night temp");
    desktop
        .reset_setting("nightlight.temperature_night")
        .await
        .expect("reset night temp");

    let favorites = desktop.setting("dock.pinned").await.expect("dock.pinned");
    let dock_baseline = string_list(&favorites);
    desktop
        .update_application_list(
            "dock.pinned",
            &dock_baseline,
            &["test-first".into(), "test-second".into()],
        )
        .await
        .expect("update dock pins");
    session.wait_for_settings_file().await;
    assert!(
        desktop
            .update_application_list("dock.pinned", &dock_baseline, &["stale".into()])
            .await
            .is_err()
    );

    let widgets = desktop.setting("bar:default:center").await.expect("bar widgets");
    let widget_baseline = string_list(&widgets);
    desktop
        .update_ordered_list(
            "bar:default:center",
            &widget_baseline,
            &["output_volume".into(), "clock".into()],
        )
        .await
        .expect("update bar widgets");
    assert_eq!(
        string_list(&desktop.setting("bar:default:center").await.unwrap()),
        vec!["output_volume".to_string(), "clock".to_string()]
    );
    assert!(
        desktop
            .update_ordered_list("bar:default:center", &widget_baseline, &["clock".into()])
            .await
            .is_err(),
        "stale bar widget baseline must be rejected"
    );

    desktop
        .update_setting("dock.enabled", &json!(true))
        .await
        .expect("enable dock");

    assert!(
        desktop
            .commit_overrides("", "[dock]\nenabled=false\n")
            .await
            .is_err()
    );

    let copy_keys = desktop.setting("keybinds.copy").await.expect("keybinds.copy");
    let key_baseline = json_array_value(&copy_keys);
    let edited = json!(["Alt+F9", "Ctrl+F8"]);
    desktop
        .update_ordered_list_json("keybinds.copy", &key_baseline, &edited)
        .await
        .expect("edit copy chord");
    assert!(
        desktop
            .update_ordered_list_json("keybinds.copy", &key_baseline, &json!(["Ctrl+c"]))
            .await
            .is_err()
    );

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
        .expect("add notification rule");
    assert!(
        desktop
            .update_ordered_list_json("notification.filter", &pristine_rules, &json!([]))
            .await
            .is_err(),
        "stale notification-rule baseline must be rejected"
    );

    let rules_baseline = json_array_value(&desktop.setting("notification.filter").await.unwrap());
    let mut invalid = rule.clone();
    invalid["match_content"] = json!("[");
    assert!(
        desktop
            .update_ordered_list_json("notification.filter", &rules_baseline, &json!([invalid]))
            .await
            .is_err()
    );

    let monitors = desktop.setting("notification.monitors").await.expect("monitors");
    let monitor_baseline = json_array_value(&monitors);
    desktop
        .update_ordered_list_json(
            "notification.monitors",
            &monitor_baseline,
            &json!(["DP-1", "eDP-1"]),
        )
        .await
        .expect("set monitor list");
    assert!(
        desktop
            .update_ordered_list_json(
                "notification.monitors",
                &monitor_baseline,
                &json!(["stale-display"]),
            )
            .await
            .is_err(),
        "stale monitor baseline must be rejected"
    );
}
