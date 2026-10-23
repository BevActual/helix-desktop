//! How the shell writes the time of day: the person's choice (Settings → Date & time) is `~/.config/helix/clock-settings.json`. This reads it,
//! tells the page when it changes (a stat once a second, nothing more), and answers the page's question at start-up. The page does the formatting.

use std::time::Duration;

use helix_config::clock_prefs::ClockFormat;
use tauri::{AppHandle, Emitter};

fn path() -> Option<std::path::PathBuf> {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(std::path::PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME").map(|h| std::path::PathBuf::from(h).join(".config"))
        })?;
    Some(base.join("helix/clock-settings.json"))
}

fn read() -> ClockFormat {
    path()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .map(|text| ClockFormat::parse(&text))
        .unwrap_or_default()
}

/// `auto`, `12` or `24`.
#[tauri::command]
pub fn clock_format() -> &'static str {
    read().name()
}

pub fn start(app: AppHandle) {
    std::thread::spawn(move || {
        let stamp = || {
            path()
                .and_then(|p| std::fs::metadata(p).ok())
                .and_then(|m| m.modified().ok())
        };
        let mut seen = stamp();
        loop {
            std::thread::sleep(Duration::from_secs(1));
            let now = stamp();
            if now == seen {
                continue;
            }
            seen = now;
            let format = read().name();
            eprintln!("CLOCK format {format}");
            let _ = app.emit("clock-format", format);
        }
    });
}
