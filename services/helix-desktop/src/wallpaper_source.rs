//! The wallpaper palette source: the palette generated from the wallpaper image (`theme.source = wallpaper`), cached by
//! path, scheme and modification time like C++ `resolveWallpaperGenerated`. `None` means "use the C++ helper".

use std::path::{Path, PathBuf};
use std::sync::Mutex;

use helix_shell_core::desktop::settings::ConfigStore;
use helix_theme::custom::{GeneratedPalette, WallpaperPalette};
use helix_theme::wallpaper::{generate, load_and_resize, Scheme};

use crate::assets_path::helix_assets_dir;

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

fn settings_wallpaper_table(config: &ConfigStore) -> Option<toml::Table> {
    let text = std::fs::read_to_string(&config.paths().settings_toml).ok()?;
    let table = text.parse::<toml::Table>().ok()?;
    table.get("wallpaper").and_then(|w| w.as_table()).cloned()
}

fn path_from_wallpaper_section(section: &toml::Table) -> Option<String> {
    section
        .get("path")
        .and_then(toml::Value::as_str)
        .map(expand_user)
        .filter(|p| !p.is_empty())
}

/// Mirrors C++ `hasConfiguredWallpaper()`.
fn has_configured_wallpaper(config: &ConfigStore, wallpaper: Option<&toml::Table>) -> bool {
    if let Some(w) = wallpaper {
        let read = |key: &str| {
            w.get(key)
                .and_then(|v| v.as_table())
                .and_then(path_from_wallpaper_section)
        };
        if read("last").is_some() || read("default").is_some() {
            return true;
        }
        if w.get("monitors")
            .and_then(|m| m.as_table())
            .is_some_and(|t| t.values().any(|v| v.as_str().is_some_and(|s| !s.is_empty())))
        {
            return true;
        }
    }
    default_wallpaper_from_config(config).is_some()
}

/// C++ `firstRunWallpaperPath()`: bundled asset until setup completes and no wallpaper is configured.
fn first_run_bundled_wallpaper(config: &ConfigStore) -> Option<String> {
    let state_dir = config.paths().settings_toml.parent()?;
    let marker = state_dir.join(".setup-complete");
    if marker.is_file() {
        return None;
    }
    let wallpaper = settings_wallpaper_table(config);
    if has_configured_wallpaper(config, wallpaper.as_ref()) {
        return None;
    }
    let bundled = helix_assets_dir()
        .map(|assets| assets.join("helix-wallpaper.png"))
        .filter(|path| path.is_file())?;
    Some(bundled.to_string_lossy().into_owned())
}

fn default_wallpaper_from_config(config: &ConfigStore) -> Option<String> {
    config
        .template_config_table()
        .get("wallpaper")
        .and_then(|w| w.get("default"))
        .and_then(|d| d.get("path"))
        .and_then(|p| p.as_str())
        .map(expand_user)
        .filter(|p| !p.is_empty())
}

/// `getPaletteWallpaperPath()`: session path, first-run bundled, then last, then default.
pub fn palette_wallpaper_path(config: &ConfigStore, session_wallpaper: &str) -> String {
    if !session_wallpaper.is_empty() {
        return expand_user(session_wallpaper);
    }
    if let Some(bundled) = first_run_bundled_wallpaper(config) {
        return bundled;
    }
    if let Some(wallpaper) = settings_wallpaper_table(config) {
        let read = |key: &str| {
            wallpaper
                .get(key)
                .and_then(|v| v.as_table())
                .and_then(path_from_wallpaper_section)
        };
        if let Some(path) = read("last").or_else(|| read("default")) {
            return path;
        }
    }
    default_wallpaper_from_config(config).unwrap_or_default()
}

fn mtime_ns(path: &Path) -> i128 {
    std::fs::metadata(path)
        .and_then(|m| m.modified())
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_nanos() as i128)
        .unwrap_or(0)
}

pub fn wallpaper_palette(config: &ConfigStore, session_wallpaper: &str) -> Option<WallpaperPalette> {
    let path = palette_wallpaper_path(config, session_wallpaper);
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

#[cfg(test)]
mod tests {
    use super::*;
    use helix_shell_core::desktop::settings::ConfigPaths;
    use std::fs;

    #[test]
    fn first_run_uses_bundled_wallpaper_when_setup_incomplete() {
        let root = std::env::temp_dir().join(format!("helix-wp-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        let assets = helix_assets_dir().expect("assets in tree");
        let bundled = assets.join("helix-wallpaper.png");
        assert!(bundled.is_file(), "missing helix-wallpaper.png in assets");

        let paths = ConfigPaths {
            config_dir: root.join("cfg"),
            settings_toml: root.join("state/settings.toml"),
        };
        fs::create_dir_all(paths.config_dir.join("helix")).unwrap();
        fs::create_dir_all(paths.settings_toml.parent().unwrap()).unwrap();
        fs::write(&paths.settings_toml, "[wallpaper]\n").unwrap();

        let config = ConfigStore::load(paths);
        let path = palette_wallpaper_path(&config, "");
        assert_eq!(path, bundled.to_string_lossy());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn session_wallpaper_overrides_settings() {
        let root = std::env::temp_dir().join(format!("helix-wp-session-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(root.join("state")).unwrap();
        let paths = ConfigPaths {
            config_dir: root.join("cfg"),
            settings_toml: root.join("state/settings.toml"),
        };
        fs::create_dir_all(paths.config_dir.join("helix")).unwrap();
        fs::write(
            &paths.settings_toml,
            "[wallpaper]\nlast = { path = \"/from/settings.png\" }\n",
        )
        .unwrap();
        fs::write(root.join("state/.setup-complete"), "").unwrap();

        let config = ConfigStore::load(paths);
        assert_eq!(
            palette_wallpaper_path(&config, "/session/wall.png"),
            "/session/wall.png"
        );
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn setup_marker_skips_bundled_wallpaper() {
        let root = std::env::temp_dir().join(format!("helix-wp-marker-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(root.join("state")).unwrap();
        let paths = ConfigPaths {
            config_dir: root.join("cfg"),
            settings_toml: root.join("state/settings.toml"),
        };
        fs::create_dir_all(paths.config_dir.join("helix")).unwrap();
        fs::write(&paths.settings_toml, "").unwrap();
        fs::write(root.join("state/.setup-complete"), "").unwrap();

        let config = ConfigStore::load(paths);
        assert!(palette_wallpaper_path(&config, "").is_empty());
        let _ = fs::remove_dir_all(&root);
    }
}
