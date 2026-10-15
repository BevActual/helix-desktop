//! Custom palette mutations (ThemeService `saveCurrentPaletteAsCustom` / `saveRoleColorAsCustom`).
//!
//! Builtin and custom palette sources with a concrete dark/light shell mode run in-process
//! through `helix-theme`. Wallpaper sources use in-process generation when the wallpaper decodes;
//! community when the cache file exists. Unreadable wallpapers or missing community cache fall
//! otherwise the call errors (optional `HELIX_THEME_PALETTE_CMD` for tests).
//! Builtin/custom sources resolve `theme.mode = auto` in-process via the day/night schedule.

use std::path::PathBuf;
use std::process::Command;

use helix_shell_core::desktop::settings::ConfigStore;
use helix_theme::custom::{
    parse_hex, save_current_as_custom, save_role_color_as_custom, ColorRole, PaletteSource,
    SaveError, ThemeSelection,
};

/// Selection state captured from config while the state lock is held.
#[derive(Clone, Debug)]
pub struct Inputs {
    local: Option<(ThemeSelection, PathBuf)>,
}

#[derive(Debug)]
pub enum Outcome {
    /// The C++ helper wrote the palette and already selected it in settings.
    Helper(Option<String>),
    /// Written in-process; the caller must select it (`theme.source` / `theme.custom_palette`).
    Local(String),
}

impl Inputs {
    pub fn capture(config: &ConfigStore, session_wallpaper: &str) -> Self {
        Self {
            local: local_selection(config, session_wallpaper),
        }
    }
}

fn local_selection(
    config: &ConfigStore,
    session_wallpaper: &str,
) -> Option<(ThemeSelection, PathBuf)> {
    // The explicit command override is a test hook for the helper; honor it as before.
    if std::env::var("HELIX_THEME_PALETTE_CMD").is_ok_and(|cmd| !cmd.trim().is_empty()) {
        return None;
    }
    let text = |id: &str| config.get(id).as_str().map(str::to_string);
    let source = match text("theme.source").as_deref() {
        Some("builtin") | None => PaletteSource::Builtin,
        Some("custom") => PaletteSource::Custom,
        Some("community") => PaletteSource::Community,
        Some("wallpaper") => PaletteSource::Wallpaper,
        Some(_) => return None,
    };
    // A wallpaper that cannot be turned into a palette is the helper's to handle.
    let wallpaper = if source == PaletteSource::Wallpaper {
        Some(crate::wallpaper_source::wallpaper_palette(
            config,
            session_wallpaper,
        )?)
    } else {
        None
    };
    let shell_light = crate::day_night::shell_palette_light(config)?;
    if source == PaletteSource::Community {
        let name = text("theme.community_palette").unwrap_or_default();
        let state_dir = config.paths().settings_toml.parent()?;
        let offline = config.get("shell.offline_mode").as_bool().unwrap_or(false);
        if !crate::palette_catalog::ensure_community_palette_cached(state_dir, &name, offline) {
            return None;
        }
    }
    let selection = ThemeSelection {
        source,
        builtin: text("theme.builtin").unwrap_or_else(|| "Helix".into()),
        custom_palette: text("theme.custom_palette").unwrap_or_default(),
        community_cache: community_cache(config, source),
        shell_light,
        wallpaper,
    };
    let dir = config.paths().config_dir.join("palettes");
    Some((selection, dir))
}

fn community_cache(config: &ConfigStore, source: PaletteSource) -> Option<PathBuf> {
    if source != PaletteSource::Community {
        return None;
    }
    let name = config
        .get("theme.community_palette")
        .as_str()
        .map(str::to_string)
        .unwrap_or_default();
    if name.is_empty() {
        return None;
    }
    let state_dir = config.paths().settings_toml.parent()?.to_path_buf();
    Some(helix_theme::custom::community_palette_cache_path(
        &state_dir, &name,
    ))
}

fn run_helper(_args: &[&str]) -> Result<Option<String>, String> {
    if let Ok(cmd) = std::env::var("HELIX_THEME_PALETTE_CMD") {
        if !cmd.trim().is_empty() {
            let status = Command::new("sh")
                .arg("-lc")
                .arg(&cmd)
                .status()
                .map_err(|error| error.to_string())?;
            if !status.success() {
                return Err("Palette could not be saved".into());
            }
            return Ok(None);
        }
    }

    Err("Palette could not be saved".into())
}

fn helper_named(args: &[&str]) -> Result<Outcome, String> {
    run_helper(args)?
        .map(|name| Outcome::Helper(Some(name)))
        .ok_or_else(|| "Palette could not be saved".into())
}

fn local_result(
    result: Result<String, SaveError>,
    helper_args: &[&str],
) -> Result<Outcome, String> {
    match result {
        Ok(name) => Ok(Outcome::Local(name)),
        Err(SaveError::WallpaperSource) => helper_named(helper_args),
        Err(SaveError::Message(message)) => Err(message),
    }
}

pub fn save_palette(inputs: &Inputs, source: &str) -> Result<Outcome, String> {
    if source != "current" && source != "wallpaper" {
        return Err("Unknown palette source".into());
    }
    let args = ["save-palette", source];
    match (&inputs.local, source) {
        (Some((selection, dir)), "current" | "wallpaper") => {
            local_result(save_current_as_custom(dir, selection), &args)
        }
        _ => helper_named(&args),
    }
}

pub fn set_role(inputs: &Inputs, role: u32, hex_color: &str) -> Result<Outcome, String> {
    let role_index = role.to_string();
    let args = ["set-role", role_index.as_str(), hex_color];
    let (Some((selection, dir)), Some(parsed)) = (&inputs.local, parse_hex(hex_color)) else {
        return helper_named(&args);
    };
    let role =
        ColorRole::from_index(role).ok_or_else(|| "error: unknown color role".to_string())?;
    local_result(
        save_role_color_as_custom(dir, selection, role, parsed),
        &args,
    )
}

pub fn set_accent(inputs: &Inputs, hex_color: &str) -> Result<Outcome, String> {
    let args = ["set-accent", hex_color];
    let (Some((selection, dir)), Some(parsed)) = (&inputs.local, parse_hex(hex_color)) else {
        return run_helper(&args).map(Outcome::Helper);
    };
    local_result(
        save_role_color_as_custom(dir, selection, ColorRole::Primary, parsed),
        &args,
    )
}

#[cfg(test)]
mod tests {
    use super::Inputs;
    use helix_shell_core::desktop::settings::{ConfigPaths, ConfigStore};
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn smoke_shaped_store() -> ConfigStore {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let home = std::env::temp_dir().join(format!("helix-theme-palette-{nanos}"));
        fs::create_dir_all(home.join("helix")).unwrap();
        fs::create_dir_all(home.join("state/helix")).unwrap();
        fs::write(
            home.join("helix/config.toml"),
            r#"[theme]
source = "builtin"
builtin = "Helix"
mode = "auto"

[theme.templates]
builtin_ids = ["helix", "qt"]

[location]
latitude = 47.6062
longitude = -122.3321
"#,
        )
        .unwrap();
        unsafe {
            std::env::remove_var("HELIX_CONFIG_HOME");
            std::env::remove_var("HELIX_STATE_HOME");
            std::env::remove_var("HELIX_THEME_PALETTE_CMD");
            std::env::set_var("XDG_CONFIG_HOME", &home);
            std::env::set_var("XDG_STATE_HOME", home.join("state"));
        }
        ConfigStore::load(ConfigPaths::from_env())
    }

    #[test]
    fn builtin_auto_mode_stays_in_process() {
        let _guard = crate::testing::ENV_LOCK.lock().unwrap();
        let store = smoke_shaped_store();
        let inputs = Inputs::capture(&store, "");
        assert!(
            inputs.local.is_some(),
            "builtin palette with theme.mode=auto should not require helix-theme-palette"
        );
    }

    #[test]
    fn community_palette_with_cache_stays_in_process() {
        let _guard = crate::testing::ENV_LOCK.lock().unwrap();
        let repo = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
        let palette_src = repo.join(
            "core/helix-theme/tests/golden/apply/community-source/statefiles/community-palettes/Oxo%20lab.json",
        );
        assert!(
            palette_src.is_file(),
            "missing community palette golden fixture"
        );

        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let home = std::env::temp_dir().join(format!("helix-theme-palette-community-{nanos}"));
        fs::create_dir_all(home.join("helix")).unwrap();
        fs::create_dir_all(home.join("state/helix/community-palettes")).unwrap();
        fs::copy(
            &palette_src,
            home.join("state/helix/community-palettes/Oxo%20lab.json"),
        )
        .unwrap();
        fs::write(
            home.join("helix/config.toml"),
            r#"[theme]
source = "community"
community_palette = "Oxo lab"
mode = "dark"
"#,
        )
        .unwrap();
        unsafe {
            std::env::remove_var("HELIX_CONFIG_HOME");
            std::env::remove_var("HELIX_STATE_HOME");
            std::env::remove_var("HELIX_THEME_PALETTE_CMD");
            std::env::set_var("XDG_CONFIG_HOME", &home);
            std::env::set_var("XDG_STATE_HOME", home.join("state"));
        }
        let store = ConfigStore::load(ConfigPaths::from_env());
        let inputs = Inputs::capture(&store, "");
        assert!(
            inputs.local.is_some(),
            "cached community palette should not require helix-theme-palette"
        );
    }
}
