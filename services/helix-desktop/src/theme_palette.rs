//! Custom palette mutations (ThemeService `saveCurrentPaletteAsCustom` / `saveRoleColorAsCustom`).
//!
//! Builtin and custom palette sources with a concrete dark/light shell mode run in-process
//! through `helix-theme`. Wallpaper sources use in-process generation when the wallpaper decodes;
//! community when the cache file exists. Unreadable wallpapers, missing community cache, and
//! `auto` mode fall back to `helix-theme-palette`.

use std::path::PathBuf;
use std::process::Command;

use helix_shell_core::desktop::settings::ConfigStore;
use helix_theme::custom::{
    save_current_as_custom, save_role_color_as_custom, ColorRole, PaletteSource, SaveError, ThemeSelection,
    parse_hex,
};

use crate::assets_path::helix_theme_palette_bin;

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

fn local_selection(config: &ConfigStore, session_wallpaper: &str) -> Option<(ThemeSelection, PathBuf)> {
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
        Some(crate::wallpaper_source::wallpaper_palette(config, session_wallpaper)?)
    } else {
        None
    };
    let mode = text("theme.mode").unwrap_or_else(|| "dark".into());
    let shell_mode = text("theme.shell_mode").unwrap_or_else(|| "follow".into());
    let effective = if shell_mode == "follow" { mode } else { shell_mode };
    let shell_light = crate::day_night::resolve_mode(config, &effective)? == "light";
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
    let name = config.get("theme.community_palette").as_str().map(str::to_string).unwrap_or_default();
    if name.is_empty() {
        return None;
    }
    let state_dir = config.paths().settings_toml.parent()?.to_path_buf();
    Some(helix_theme::custom::community_palette_cache_path(&state_dir, &name))
}

fn run_helper(args: &[&str]) -> Result<Option<String>, String> {
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

    let bin = helix_theme_palette_bin()
        .ok_or_else(|| "Palette could not be saved".to_string())?;
    tracing::info!(bin = %bin.display(), args = ?args, "theme palette: using helix-theme-palette");
    let output = Command::new(&bin)
        .args(args)
        .output()
        .map_err(|error| error.to_string())?;
    if output.status.success() {
        let name = String::from_utf8_lossy(&output.stdout).trim().to_string();
        return Ok(if name.is_empty() { None } else { Some(name) });
    }
    let stderr = String::from_utf8_lossy(&output.stderr);
    let message = stderr.trim();
    if message.is_empty() {
        Err("Palette could not be saved".into())
    } else {
        Err(message.lines().last().unwrap_or(message).to_string())
    }
}

fn helper_named(args: &[&str]) -> Result<Outcome, String> {
    run_helper(args)?
        .map(|name| Outcome::Helper(Some(name)))
        .ok_or_else(|| "Palette could not be saved".into())
}

fn local_result(result: Result<String, SaveError>, helper_args: &[&str]) -> Result<Outcome, String> {
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
    let role = ColorRole::from_index(role).ok_or_else(|| "error: unknown color role".to_string())?;
    local_result(save_role_color_as_custom(dir, selection, role, parsed), &args)
}

pub fn set_accent(inputs: &Inputs, hex_color: &str) -> Result<Outcome, String> {
    let args = ["set-accent", hex_color];
    let (Some((selection, dir)), Some(parsed)) = (&inputs.local, parse_hex(hex_color)) else {
        return run_helper(&args).map(|name| Outcome::Helper(name));
    };
    local_result(save_role_color_as_custom(dir, selection, ColorRole::Primary, parsed), &args)
}
