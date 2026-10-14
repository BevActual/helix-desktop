//! Resolves `auto` theme modes against the day/night schedule (`helix-theme::schedule`).
//! Uses configured coordinates / custom times only, like the one-shot C++ helpers did.

use helix_shell_core::ConfigStore;
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
            gmt_offset_secs: i64::from(tm.tm_gmtoff),
        }
    }
}

fn location(config: &ConfigStore) -> LocationConfig {
    LocationConfig {
        custom_schedule: config.get("location.custom_schedule").as_bool().unwrap_or(false),
        sunset: config.get("location.sunset").as_str().unwrap_or("").to_string(),
        sunrise: config.get("location.sunrise").as_str().unwrap_or("").to_string(),
        latitude: config.get("location.latitude").as_f64(),
        longitude: config.get("location.longitude").as_f64(),
    }
}

/// Whether it is currently night per the configured schedule.
pub fn is_night(config: &ConfigStore) -> bool {
    evaluate(&location(config), local_time()).night
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
