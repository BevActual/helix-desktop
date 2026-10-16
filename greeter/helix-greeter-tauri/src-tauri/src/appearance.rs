//! The appearance payload written by `helix-greeter-apply-appearance`.
//! The greeter only reads files inside its state directory.

use std::fs;
use std::path::{Path, PathBuf};

use serde::Deserialize;

#[derive(Clone, Debug, PartialEq, Deserialize)]
pub struct Colors {
    pub background: String,
    pub on_background: String,
    pub surface: String,
    pub on_surface: String,
    pub primary: String,
    pub on_primary: String,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Appearance {
    pub dark: bool,
    pub font: String,
    pub ui_scale: f64,
    pub corner_radius_scale: f64,
    pub transparency_mode: String,
    pub refraction: bool,
    pub high_contrast: bool,
    pub reduced_motion: bool,
    pub colors: Option<Colors>,
    /// Absolute path of the wallpaper image inside the state directory.
    pub wallpaper: Option<PathBuf>,
}

impl Default for Appearance {
    fn default() -> Self {
        Self {
            dark: true,
            font: String::new(),
            ui_scale: 1.0,
            corner_radius_scale: 1.0,
            transparency_mode: "soft".into(),
            refraction: false,
            high_contrast: false,
            reduced_motion: false,
            colors: None,
            wallpaper: None,
        }
    }
}

#[derive(Deserialize)]
struct FilePayload {
    dark: Option<bool>,
    font: Option<String>,
    ui_scale: Option<f64>,
    corner_radius_scale: Option<f64>,
    transparency_mode: Option<String>,
    refraction: Option<bool>,
    high_contrast: Option<bool>,
    reduced_motion: Option<bool>,
    wallpaper: Option<String>,
    colors: Option<Colors>,
}

pub fn state_dir() -> PathBuf {
    std::env::var_os("HELIX_GREETER_STATE")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/var/lib/helix-greeter"))
}

pub fn load(dir: &Path) -> Appearance {
    let Ok(text) = fs::read_to_string(dir.join("appearance.json")) else {
        return Appearance::default();
    };
    if text.len() > 64 * 1024 {
        return Appearance::default();
    }
    let Ok(payload) = serde_json::from_str::<FilePayload>(&text) else {
        return Appearance::default();
    };
    let mut appearance = Appearance {
        dark: payload.dark.unwrap_or(true),
        font: payload
            .font
            .filter(|font| font_ok(font))
            .unwrap_or_default(),
        ui_scale: payload
            .ui_scale
            .filter(|value| value.is_finite())
            .unwrap_or(1.0)
            .clamp(0.5, 3.0),
        corner_radius_scale: payload
            .corner_radius_scale
            .filter(|value| value.is_finite())
            .unwrap_or(1.0)
            .clamp(0.0, 2.0),
        transparency_mode: match payload.transparency_mode.as_deref() {
            Some("solid") => "solid".into(),
            Some("glass") => "glass".into(),
            _ => "soft".into(),
        },
        refraction: payload.refraction.unwrap_or(false),
        high_contrast: payload.high_contrast.unwrap_or(false),
        reduced_motion: payload.reduced_motion.unwrap_or(false),
        colors: payload.colors.filter(colors_ok),
        wallpaper: None,
    };
    if let Some(name) = payload.wallpaper.filter(|name| wallpaper_name_ok(name)) {
        let path = dir.join(name);
        if path.is_file() {
            appearance.wallpaper = Some(path);
        }
    }
    appearance
}

fn font_ok(font: &str) -> bool {
    !font.is_empty()
        && font.len() <= 80
        && font
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, ' ' | ',' | '-' | '.'))
}

fn hex_ok(value: &str) -> bool {
    let Some(digits) = value.strip_prefix('#') else {
        return false;
    };
    digits.len() == 6 && digits.bytes().all(|byte| byte.is_ascii_hexdigit())
}

fn colors_ok(colors: &Colors) -> bool {
    hex_ok(&colors.background)
        && hex_ok(&colors.on_background)
        && hex_ok(&colors.surface)
        && hex_ok(&colors.on_surface)
        && hex_ok(&colors.primary)
        && hex_ok(&colors.on_primary)
}

fn wallpaper_name_ok(name: &str) -> bool {
    let path = Path::new(name);
    path.components().count() == 1
        && matches!(
            path.extension().and_then(|ext| ext.to_str()),
            Some("png" | "jpg" | "jpeg" | "webp")
        )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn missing_file_stays_dark() {
        let dir =
            std::env::temp_dir().join(format!("helix-greeter-missing-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let appearance = load(&dir);
        assert!(appearance.dark);
        assert!(appearance.colors.is_none());
        assert!(appearance.wallpaper.is_none());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn reads_colors_and_refuses_a_wallpaper_path() {
        let dir = std::env::temp_dir().join(format!("helix-greeter-read-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("wallpaper.png"), b"png").unwrap();
        fs::write(
            dir.join("appearance.json"),
            r##"{"dark":false,"font":"Inter","ui_scale":1.5,"corner_radius_scale":0.5,"transparency_mode":"glass","refraction":true,"high_contrast":true,"reduced_motion":true,"wallpaper":"../wallpaper.png","colors":{"background":"#111111","on_background":"#eeeeee","surface":"#222222","on_surface":"#dddddd","primary":"#3366cc","on_primary":"#ffffff"}}"##,
        )
        .unwrap();
        let appearance = load(&dir);
        assert!(!appearance.dark);
        assert_eq!(appearance.font, "Inter");
        assert_eq!(appearance.transparency_mode, "glass");
        assert!(appearance.high_contrast);
        assert!(appearance.wallpaper.is_none());
        assert_eq!(appearance.colors.unwrap().primary, "#3366cc");
        let _ = fs::remove_dir_all(&dir);
    }
}
