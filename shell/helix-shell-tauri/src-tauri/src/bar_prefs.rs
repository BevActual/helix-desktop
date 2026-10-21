//! What the top bar shows: the person's choices (Settings → Top bar) are `~/.config/helix/bar-settings.json`. This reads them, tells the page when they
//! change (a stat once a second, nothing more), and answers the page's question at start-up. The page does the hiding.

use std::time::Duration;

use helix_config::bar_prefs::BarPrefs;
use tauri::{AppHandle, Emitter};

fn path() -> Option<std::path::PathBuf> {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(std::path::PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME").map(|h| std::path::PathBuf::from(h).join(".config"))
        })?;
    Some(base.join("helix/bar-settings.json"))
}

fn read() -> BarPrefs {
    path()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .map(|text| BarPrefs::parse(&text))
        .unwrap_or_default()
}

/// Every item and whether it is shown, as JSON: `{"apps":true,"seconds":false,…}`.
#[tauri::command]
pub fn bar_prefs() -> String {
    read().effective_json()
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
            let prefs = read().effective_json();
            eprintln!("BAR prefs {prefs}");
            let _ = app.emit("bar-prefs", prefs);
        }
    });
}
