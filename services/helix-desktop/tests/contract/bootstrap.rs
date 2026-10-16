use helix_desktop_api::{Desktop, Domain};
use serde_json::json;

pub async fn run(desktop: &Desktop) {
    let config = desktop
        .snapshot(Domain::Configuration)
        .await
        .expect("config snap");
    assert_eq!(config.get("helix-desktop"), Some(&json!(false)));
    assert_eq!(config.get("network").and_then(|v| v.as_bool()), Some(false));
    assert_eq!(
        config.get("bluetooth").and_then(|v| v.as_bool()),
        Some(false)
    );
    let system = config.get("system").expect("system");
    assert!(system.get("os").and_then(|v| v.as_str()).is_some());
    assert!(system
        .get("architecture")
        .and_then(|v| v.as_str())
        .is_some());

    let displays = desktop.snapshot(Domain::Displays).await.expect("displays");
    assert_eq!(
        displays.get("available").and_then(|v| v.as_bool()),
        Some(false)
    );
    assert!(displays
        .get("outputs")
        .and_then(|v| v.as_array())
        .is_some_and(|o| o.is_empty()));
    assert!(desktop
        .action(Domain::Displays, "unsupported", &json!({}))
        .await
        .is_err());

    let appearance = desktop
        .snapshot(Domain::Appearance)
        .await
        .expect("appearance");
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
            let accents = variant
                .get("accents")
                .and_then(|v| v.as_array())
                .expect("accents");
            assert!(!accents.is_empty());
        }
    }
    assert_eq!(
        appearance.get("wallhaven-online").and_then(|v| v.as_bool()),
        Some(false)
    );

    let settings = desktop.settings().await.expect("settings");
    for entry in settings.as_array().expect("settings array") {
        let page = entry.get("page").and_then(|v| v.as_str()).unwrap_or("");
        assert!(
            page != "notifications"
                && page != "accessibility"
                && page != "keyboard"
                && page != "system"
                && page != "power"
                && page != "desktop-dock"
                && (page != "privacy-security"
                    || entry
                        .get("id")
                        .and_then(|v| v.as_str())
                        .is_some_and(|id| id.starts_with("ai.")))
        );
    }

    assert!(desktop
        .update_setting("notification.show_actions", &json!(false))
        .await
        .is_err());
    assert!(desktop
        .action(
            Domain::Network,
            "connect",
            &json!({"path": "/missing", "secret": "never-log-this-password"}),
        )
        .await
        .is_err());
    assert!(desktop.audio_start_input_meter(999).await.is_err());
    desktop.audio_stop_input_meter().await.expect("stop meter");
}
