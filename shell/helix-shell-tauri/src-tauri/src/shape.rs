//! How round the shell's own corners are: the person's choice (Settings → Appearance → Corner roundness) is a scale in
//! `~/.config/helix/shape-settings.json`. This reads it, tells the page when it changes (a stat once a second) and answers at start-up. The page puts the
//! scale on the root element as `--shape`, which the bar, dock, panels and toasts multiply their radii by.

use std::time::Duration;

use tauri::{AppHandle, Emitter};

fn path() -> Option<std::path::PathBuf> {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(std::path::PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME").map(|h| std::path::PathBuf::from(h).join(".config"))
        })?;
    Some(base.join("helix/shape-settings.json"))
}

fn read() -> (f64, f64) {
    let text = path()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .unwrap_or_default();
    (
        if text.is_empty() {
            1.0
        } else {
            helix_config::shape_prefs::parse_scale(&text)
        },
        helix_config::shape_prefs::parse_glass(&text),
    )
}

/// The corner scale (0 square to 2, 1 as designed) and the glass level (0 clear to 1 tinted).
#[tauri::command]
pub fn shape_scale() -> (f64, f64) {
    read()
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
            let (scale, glass) = read();
            eprintln!("SHAPE scale={scale} glass={glass}");
            let _ = app.emit("shape-scale", (scale, glass));
        }
    });
}
