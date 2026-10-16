//! Resolves `auto` theme modes against the day/night schedule (`helix-theme::schedule`).
//! Uses configured coordinates / custom times only, like the one-shot C++ helpers did.

use helix_config::settings::ConfigStore;
use helix_theme::schedule::{evaluate, LocalTime, LocationConfig};

fn local_time() -> LocalTime {
    // SAFETY: `time` and `localtime_r` are called with valid pointers; `tm` is plain data.
    unsafe {
        let now = libc::time(std::ptr::null_mut());
        let mut tm: libc::tm = std::mem::zeroed();
        libc::localtime_r(&now, &mut tm);
        LocalTime {
            minute_of_day: tm.tm_hour * 60 + tm.tm_min,
            second: tm.tm_sec,
            day_of_year: tm.tm_yday + 1,
            gmt_offset_secs: tm.tm_gmtoff,
        }
    }
}

fn location(config: &ConfigStore) -> LocationConfig {
    LocationConfig {
        custom_schedule: config
            .get("location.custom_schedule")
            .as_bool()
            .unwrap_or(false),
        sunset: config
            .get("location.sunset")
            .as_str()
            .unwrap_or("")
            .to_string(),
        sunrise: config
            .get("location.sunrise")
            .as_str()
            .unwrap_or("")
            .to_string(),
        latitude: config.get("location.latitude").as_f64(),
        longitude: config.get("location.longitude").as_f64(),
    }
}

/// Whether it is currently night per the configured schedule.
pub fn is_night(config: &ConfigStore) -> bool {
    evaluate(&location(config), local_time()).night
}

/// `[theme].shell_mode` when not `follow` pins the shell palette; app-facing `theme.mode` is unchanged.
pub fn effective_shell_mode<'a>(app_mode: &'a str, shell_mode: &'a str) -> &'a str {
    if shell_mode == "follow" || shell_mode.is_empty() {
        app_mode
    } else {
        shell_mode
    }
}

/// App-facing mode for templates and GTK (`ThemeService::resolvedMode` / resolved callback).
pub fn resolved_app_mode(config: &ConfigStore) -> Option<&'static str> {
    resolve_mode(config, config.get("theme.mode").as_str().unwrap_or("dark"))
}

/// Whether Helix chrome uses the light palette (`ThemeService::isLightMode`).
pub fn shell_palette_light(config: &ConfigStore) -> Option<bool> {
    let shell_mode = config
        .get("theme.shell_mode")
        .as_str()
        .map(str::to_string)
        .unwrap_or_else(|| "follow".into());
    let app_mode = config
        .get("theme.mode")
        .as_str()
        .map(str::to_string)
        .unwrap_or_else(|| "dark".into());
    let effective = effective_shell_mode(&app_mode, &shell_mode);
    resolve_mode(config, effective).map(|mode| mode == "light")
}

/// Resolved shell mode string (`ThemeService::resolvedShellMode`).
#[cfg(test)]
pub fn resolved_shell_mode_label(config: &ConfigStore) -> Option<&'static str> {
    let shell_mode = config
        .get("theme.shell_mode")
        .as_str()
        .map(str::to_string)
        .unwrap_or_else(|| "follow".into());
    let app_mode = config
        .get("theme.mode")
        .as_str()
        .map(str::to_string)
        .unwrap_or_else(|| "dark".into());
    resolve_mode(config, effective_shell_mode(&app_mode, &shell_mode))
}

/// Resolves `dark` / `light` / `auto` to `dark` or `light`; anything else is `None`.
pub fn resolve_mode(config: &ConfigStore, configured: &str) -> Option<&'static str> {
    match configured {
        "dark" => Some("dark"),
        "light" => Some("light"),
        "auto" => Some(if is_night(config) { "dark" } else { "light" }),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use helix_config::settings::{ConfigPaths, ConfigStore};

    fn store() -> ConfigStore {
        let root = std::env::temp_dir().join(format!("helix-shell-mode-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(root.join("cfg")).unwrap();
        std::fs::create_dir_all(root.join("state")).unwrap();
        ConfigStore::load(ConfigPaths {
            config_dir: root.join("cfg"),
            settings_toml: root.join("state/settings.toml"),
        })
    }

    #[test]
    fn shell_mode_matches_cpp_theme_service() {
        use serde_json::json;
        let mut config = store();
        config.set("theme.mode", json!("light")).unwrap();
        config.set("theme.shell_mode", json!("follow")).unwrap();
        assert_eq!(resolved_app_mode(&config), Some("light"));
        assert_eq!(resolved_shell_mode_label(&config), Some("light"));
        assert_eq!(shell_palette_light(&config), Some(true));

        config.set("theme.mode", json!("light")).unwrap();
        config.set("theme.shell_mode", json!("dark")).unwrap();
        assert_eq!(resolved_app_mode(&config), Some("light"));
        assert_eq!(resolved_shell_mode_label(&config), Some("dark"));
        assert_eq!(shell_palette_light(&config), Some(false));

        config.set("theme.mode", json!("dark")).unwrap();
        config.set("theme.shell_mode", json!("light")).unwrap();
        assert_eq!(resolved_app_mode(&config), Some("dark"));
        assert_eq!(resolved_shell_mode_label(&config), Some("light"));
        assert_eq!(shell_palette_light(&config), Some(true));

        config.set("theme.mode", json!("dark")).unwrap();
        config.reset("theme.shell_mode").unwrap();
        assert_eq!(resolved_app_mode(&config), Some("dark"));
        assert_eq!(resolved_shell_mode_label(&config), Some("dark"));
        assert_eq!(shell_palette_light(&config), Some(false));
    }
}
