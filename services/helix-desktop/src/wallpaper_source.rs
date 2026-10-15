//! The wallpaper palette source: the palette generated from the wallpaper image (`theme.source = wallpaper`), cached by
//! path, scheme and modification time like C++ `resolveWallpaperGenerated`. `None` means "use the C++ helper".

use std::path::{Path, PathBuf};
use std::sync::Mutex;

use helix_shell_core::desktop::settings::ConfigStore;
use helix_theme::custom::{GeneratedPalette, WallpaperPalette};
use helix_theme::wallpaper::{generate, load_and_resize, Scheme};

struct Cached {
    path: String,
    scheme: String,
    mtime_ns: i128,
    generated: GeneratedPalette,
}

static CACHE: Mutex<Option<Cached>> = Mutex::new(None);

fn expand_user(path: &str) -> String {
    match path.strip_prefix("~/") {
        Some(rest) => std::env::var("HOME")
            .map(|home| format!("{home}/{rest}"))
            .unwrap_or_else(|_| path.to_string()),
        None => path.to_string(),
    }
}

/// `getPaletteWallpaperPath()`: the last wallpaper set, else the default. Empty when none is configured (including the
/// first-run bundled wallpaper, which the helper handles).
pub fn palette_wallpaper_path(config: &ConfigStore) -> String {
    let Ok(text) = std::fs::read_to_string(&config.paths().settings_toml) else {
        return String::new();
    };
    let Ok(table) = text.parse::<toml::Table>() else {
        return String::new();
    };
    let read = |which: &str| {
        table
            .get("wallpaper")
            .and_then(|w| w.get(which))
            .and_then(|w| w.get("path"))
            .and_then(toml::Value::as_str)
            .map(expand_user)
    };
    read("last").or_else(|| read("default")).unwrap_or_default()
}

fn mtime_ns(path: &Path) -> i128 {
    std::fs::metadata(path)
        .and_then(|m| m.modified())
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_nanos() as i128)
        .unwrap_or(0)
}

pub fn wallpaper_palette(config: &ConfigStore) -> Option<WallpaperPalette> {
    let path = palette_wallpaper_path(config);
    if path.is_empty() {
        return None;
    }
    let scheme_name = config
        .get("theme.wallpaper_scheme")
        .as_str()
        .map(str::to_string)
        .unwrap_or_else(|| "m3-content".to_string());
    let scheme = Scheme::parse(&scheme_name).unwrap_or(Scheme::Content);

    let mtime = mtime_ns(&PathBuf::from(&path));
    {
        let cache = CACHE.lock().ok()?;
        if let Some(hit) = cache.as_ref() {
            if mtime != 0 && hit.path == path && hit.scheme == scheme_name && hit.mtime_ns == mtime {
                return Some(WallpaperPalette {
                    generated: hit.generated.clone(),
                    path,
                    scheme: scheme_name,
                });
            }
        }
    }
    let rgb = match load_and_resize(Path::new(&path), scheme) {
        Ok(rgb) => rgb,
        Err(error) => {
            tracing::debug!(%error, %path, "wallpaper palette: image could not be loaded");
            return None;
        }
    };
    let generated = match generate(&rgb, scheme) {
        Ok(generated) => generated,
        Err(error) => {
            tracing::warn!(%error, "failed to generate palette from wallpaper");
            return None;
        }
    };
    if mtime != 0 {
        if let Ok(mut cache) = CACHE.lock() {
            *cache = Some(Cached {
                path: path.clone(),
                scheme: scheme_name.clone(),
                mtime_ns: mtime,
                generated: generated.clone(),
            });
        }
    }
    Some(WallpaperPalette { generated, path, scheme: scheme_name })
}
