//! Bootstrap Configuration1 catalog: definitions + TOML load/save for the settings the
//! Tauri shell and Settings app need today. Full C++ catalog ports later.

use std::borrow::Cow;
use std::fs;
use std::path::{Path, PathBuf};

use serde_json::{json, Value};
use toml::Value as Toml;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SettingKind {
    Boolean,
    Number,
    Integer,
    String,
    Mode,
    Material,
    /// Desktop-entry id list (`dock.pinned`, launcher pins).
    ApplicationList,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum DefaultValue {
    Bool(bool),
    Number(f64),
    Str(&'static str),
    EmptyArray,
}

impl DefaultValue {
    pub fn to_json(self) -> Value {
        match self {
            Self::Bool(flag) => Value::Bool(flag),
            Self::Number(number) => json!(number),
            Self::Str(text) => Value::String(text.into()),
            Self::EmptyArray => Value::Array(Vec::new()),
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct SettingDef {
    pub id: &'static str,
    pub page: &'static str,
    pub title: &'static str,
    pub group: &'static str,
    pub kind: SettingKind,
    pub desktop: bool,
    pub minimum: f64,
    pub maximum: f64,
    pub default: DefaultValue,
}

impl SettingDef {
    pub fn type_name(&self) -> &'static str {
        match self.kind {
            SettingKind::Boolean => "boolean",
            SettingKind::Number => "number",
            SettingKind::Integer => "integer",
            SettingKind::String => "string",
            SettingKind::Mode => "mode",
            SettingKind::Material => "material",
            SettingKind::ApplicationList => "application-list",
        }
    }

    pub fn options(&self) -> Option<(&'static [&'static str], &'static [&'static str])> {
        match self.kind {
            SettingKind::Mode => Some((&["dark", "light", "auto"], &["Dark", "Light", "Automatic"])),
            SettingKind::Material => Some((&["solid", "soft", "glass"], &["Solid", "Soft", "Glass"])),
            _ => None,
        }
    }
}

/// Subset needed for shell volume + Settings appearance/accessibility pages.
pub const BOOTSTRAP: &[SettingDef] = &[
    SettingDef {
        id: "theme.mode",
        page: "appearance",
        title: "Appearance",
        group: "Colors",
        kind: SettingKind::Mode,
        desktop: false,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("dark"),
    },
    SettingDef {
        id: "theme.pure_black_dark",
        page: "appearance",
        title: "Pure black dark surfaces",
        group: "Colors",
        kind: SettingKind::Boolean,
        desktop: false,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "theme.source",
        page: "appearance",
        title: "Palette source",
        group: "Colors",
        kind: SettingKind::String,
        desktop: false,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("builtin"),
    },
    SettingDef {
        id: "shell.font_family",
        page: "appearance",
        title: "Interface font",
        group: "Typography",
        kind: SettingKind::String,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str(""),
    },
    SettingDef {
        id: "shell.panel.borders",
        page: "appearance",
        title: "Panel outlines",
        group: "Shell chrome",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "shell.panel.shadow",
        page: "appearance",
        title: "Panel shadows",
        group: "Shell chrome",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "shell.animation.speed",
        page: "accessibility",
        title: "Animation speed",
        group: "Motion",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.1,
        maximum: 4.0,
        default: DefaultValue::Number(1.0),
    },
    SettingDef {
        id: "nightlight.enabled",
        page: "displays",
        title: "Night Light",
        group: "Night Light",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "nightlight.force",
        page: "displays",
        title: "Keep Night Light on",
        group: "Night Light",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "nightlight.temperature_day",
        page: "displays",
        title: "Day temperature (Kelvin)",
        group: "Night Light",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 1100.0,
        maximum: 25000.0,
        default: DefaultValue::Number(6500.0),
    },
    SettingDef {
        id: "nightlight.temperature_night",
        page: "displays",
        title: "Night temperature (Kelvin)",
        group: "Night Light",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 1000.0,
        maximum: 24900.0,
        default: DefaultValue::Number(4000.0),
    },
    SettingDef {
        id: "theme.builtin",
        page: "appearance",
        title: "Palette",
        group: "Colors",
        kind: SettingKind::String,
        desktop: false,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("Helix"),
    },
    SettingDef {
        id: "theme.community_palette",
        page: "appearance",
        title: "Community palette",
        group: "Colors",
        kind: SettingKind::String,
        desktop: false,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str(""),
    },
    SettingDef {
        id: "theme.custom_palette",
        page: "appearance",
        title: "Custom palette",
        group: "Colors",
        kind: SettingKind::String,
        desktop: false,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str(""),
    },
    SettingDef {
        id: "wallpaper.enabled",
        page: "appearance",
        title: "Show desktop wallpaper",
        group: "Wallpaper",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "backdrop.enabled",
        page: "appearance",
        title: "Dim wallpaper behind windows",
        group: "Backdrop",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "backdrop.blur_intensity",
        page: "appearance",
        title: "Backdrop blur",
        group: "Backdrop",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.0,
        maximum: 1.0,
        default: DefaultValue::Number(0.5),
    },
    SettingDef {
        id: "backdrop.tint_intensity",
        page: "appearance",
        title: "Backdrop tint",
        group: "Backdrop",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.0,
        maximum: 1.0,
        default: DefaultValue::Number(0.3),
    },
    SettingDef {
        id: "shell.panel.list_item_background",
        page: "appearance",
        title: "List item backgrounds",
        group: "Shell chrome",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "shell.button_borders",
        page: "appearance",
        title: "Button outlines",
        group: "Borders",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "shell.input_borders",
        page: "appearance",
        title: "Field outlines",
        group: "Borders",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "shell.popup_borders",
        page: "appearance",
        title: "Menu outlines",
        group: "Borders",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "shell.card_borders",
        page: "appearance",
        title: "Card outlines",
        group: "Borders",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "shell.popup_shadows",
        page: "appearance",
        title: "Menu shadows",
        group: "Shadows",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "shell.offline_mode",
        page: "privacy-security",
        title: "Restrict shell network requests",
        group: "Network Privacy",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "shell.polkit_agent",
        page: "privacy-security",
        title: "Handle authentication dialogs",
        group: "Authentication",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    // Models & routing — managed in Mobius (inference-routing.toml); mirrored in C++ settings_catalog for GetSettings search.
    SettingDef {
        id: "ai.act_model",
        page: "privacy-security",
        title: "On-device act model (GGUF id)",
        group: "Models & routing",
        kind: SettingKind::String,
        desktop: false,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str(""),
    },
    SettingDef {
        id: "ai.chat_model",
        page: "privacy-security",
        title: "On-device chat model (GGUF id)",
        group: "Models & routing",
        kind: SettingKind::String,
        desktop: false,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str(""),
    },
    SettingDef {
        id: "ai.lan_endpoint",
        page: "privacy-security",
        title: "LAN OpenAI-compatible endpoint",
        group: "Models & routing",
        kind: SettingKind::String,
        desktop: false,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str(""),
    },
    SettingDef {
        id: "ai.allow_remote_on_battery_saver",
        page: "privacy-security",
        title: "Allow LAN/cloud on battery saver",
        group: "Models & routing",
        kind: SettingKind::Boolean,
        desktop: false,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "shell.clipboard_enabled",
        page: "privacy-security",
        title: "Keep clipboard history",
        group: "Clipboard",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "lockscreen.enabled",
        page: "privacy-security",
        title: "Show lock screen",
        group: "Lock Screen",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "battery.warning_threshold",
        page: "power",
        title: "Low battery warning (%)",
        group: "Power",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 100.0,
        default: DefaultValue::Number(15.0),
    },
    SettingDef {
        id: "notification.enable_daemon",
        page: "notifications",
        title: "Enable Helix notifications",
        group: "Notifications",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "notification.show_app_name",
        page: "notifications",
        title: "Show application names",
        group: "Popups",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "notification.max_visible",
        page: "notifications",
        title: "Maximum visible popups",
        group: "Popups",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 20.0,
        default: DefaultValue::Number(3.0),
    },
    SettingDef {
        id: "control_center.width",
        page: "desktop-dock",
        title: "Window width",
        group: "Control Center",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 600.0,
        maximum: 1600.0,
        default: DefaultValue::Number(860.0),
    },
    SettingDef {
        id: "control_center.height",
        page: "desktop-dock",
        title: "Window height",
        group: "Control Center",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 480.0,
        maximum: 1200.0,
        default: DefaultValue::Number(720.0),
    },
    SettingDef {
        id: "control_center.show_session_button",
        page: "desktop-dock",
        title: "Show session actions",
        group: "Control Center",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "hot_corners.enabled",
        page: "desktop-dock",
        title: "Enable hot corners",
        group: "Hot Corners",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "dock.auto_hide",
        page: "desktop-dock",
        title: "Automatically hide",
        group: "Dock",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "dock.pinned",
        page: "desktop-dock",
        title: "Pinned dock applications",
        group: "Dock Favorites",
        kind: SettingKind::ApplicationList,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::EmptyArray,
    },
    SettingDef {
        id: "shell.launcher.pinned",
        page: "desktop-dock",
        title: "Pinned launcher applications",
        group: "Launcher Favorites",
        kind: SettingKind::ApplicationList,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::EmptyArray,
    },
    SettingDef {
        id: "osd.enabled",
        page: "desktop-dock",
        title: "Show on-screen indicators",
        group: "On-Screen Indicators",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "osd.scale",
        page: "desktop-dock",
        title: "Indicator scale",
        group: "On-Screen Indicators",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.5,
        maximum: 2.5,
        default: DefaultValue::Number(1.0),
    },
    SettingDef {
        id: "osd.kinds.volume",
        page: "desktop-dock",
        title: "Volume indicators",
        group: "Indicator Events",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "osd.kinds.brightness",
        page: "desktop-dock",
        title: "Brightness",
        group: "Indicator Events",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "osd.kinds.nightlight",
        page: "desktop-dock",
        title: "Night Light",
        group: "Indicator Events",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "accessibility.high_contrast",
        page: "accessibility",
        title: "High contrast",
        group: "Presentation",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "accessibility.ui_scale",
        page: "accessibility",
        title: "Interface scale",
        group: "Presentation",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.5,
        maximum: 3.0,
        default: DefaultValue::Number(1.0),
    },
    SettingDef {
        id: "audio.sound_volume",
        page: "sound",
        title: "Sound volume",
        group: "Sounds",
        kind: SettingKind::Number,
        desktop: false,
        minimum: 0.0,
        maximum: 1.0,
        default: DefaultValue::Number(0.5),
    },
    SettingDef {
        id: "audio.enable_sounds",
        page: "sound",
        title: "Interface sounds",
        group: "Sounds",
        kind: SettingKind::Boolean,
        desktop: false,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "audio.enable_overdrive",
        page: "sound",
        title: "Allow volume above 100%",
        group: "Sounds",
        kind: SettingKind::Boolean,
        desktop: false,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "shell.panel.transparency_mode",
        page: "appearance",
        title: "Shell material",
        group: "Shell chrome",
        kind: SettingKind::Material,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("soft"),
    },
    SettingDef {
        id: "shell.panel.glass",
        page: "appearance",
        title: "Glass amount",
        group: "Shell chrome",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.0,
        maximum: 1.0,
        default: DefaultValue::Number(0.5),
    },
    SettingDef {
        id: "shell.panel.refraction",
        page: "appearance",
        title: "Refraction",
        group: "Shell chrome",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "shell.corner_radius_scale",
        page: "appearance",
        title: "Corner roundness",
        group: "Shape",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.0,
        maximum: 2.0,
        default: DefaultValue::Number(1.0),
    },
    SettingDef {
        id: "shell.animation.enabled",
        page: "accessibility",
        title: "Animations",
        group: "Motion",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
];

pub fn find_def(id: &str) -> Option<&'static SettingDef> {
    BOOTSTRAP.iter().find(|setting| setting.id == id)
}

#[derive(Clone, Debug)]
pub struct ConfigPaths {
    pub config_dir: PathBuf,
    pub settings_toml: PathBuf,
}

impl ConfigPaths {
    pub fn from_env() -> Self {
        let config_dir = named_dir("HELIX_CONFIG_HOME", "XDG_CONFIG_HOME", ".config", "helix");
        let state_dir = named_dir("HELIX_STATE_HOME", "XDG_STATE_HOME", ".local/state", "helix");
        Self {
            config_dir,
            settings_toml: state_dir.join("settings.toml"),
        }
    }
}

fn named_dir(helix_env: &str, xdg_env: &str, home_suffix: &str, leaf: &str) -> PathBuf {
    if let Ok(value) = std::env::var(helix_env) {
        if !value.is_empty() {
            return PathBuf::from(value);
        }
    }
    if let Ok(value) = std::env::var(xdg_env) {
        if !value.is_empty() {
            return PathBuf::from(value).join(leaf);
        }
    }
    if let Ok(home) = std::env::var("HOME") {
        return PathBuf::from(home).join(home_suffix).join(leaf);
    }
    PathBuf::from(leaf)
}

#[derive(Clone, Debug)]
pub struct ConfigStore {
    /// Merged base config (config.toml + includes under config dir).
    base: Toml,
    /// User overrides from state `settings.toml`.
    overrides: Toml,
    paths: ConfigPaths,
}

impl ConfigStore {
    pub fn load(paths: ConfigPaths) -> Self {
        let base = load_merged_config(&paths.config_dir);
        let overrides = read_toml_file(&paths.settings_toml).unwrap_or_else(|| Toml::Table(Default::default()));
        Self {
            base,
            overrides,
            paths,
        }
    }

    pub fn paths(&self) -> &ConfigPaths {
        &self.paths
    }

    pub fn get(&self, id: &str) -> Value {
        let def = match find_def(id) {
            Some(def) => def,
            None => return Value::Null,
        };
        if id.starts_with("ai.") {
            let mirror = crate::apps::inference_routing::settings_mirror();
            return match id {
                "ai.act_model" => Value::String(mirror.act_model),
                "ai.chat_model" => Value::String(mirror.chat_model),
                "ai.lan_endpoint" => Value::String(mirror.lan_endpoint),
                "ai.allow_remote_on_battery_saver" => Value::Bool(mirror.allow_remote_on_battery_saver),
                _ => def.default.to_json(),
            };
        }
        if let Some(value) = toml_get(&self.overrides, id) {
            return toml_to_json(value, def);
        }
        if let Some(value) = toml_get(&self.base, id) {
            return toml_to_json(value, def);
        }
        def.default.to_json()
    }

    pub fn overridden(&self, id: &str) -> bool {
        if id.starts_with("ai.") {
            return false;
        }
        toml_get(&self.overrides, id).is_some()
    }

    pub fn set(&mut self, id: &str, value: Value) -> Result<(), String> {
        if id.starts_with("ai.") {
            return Err("Model routing is managed in Mobius, not Helix Settings.".into());
        }
        let def = find_def(id).ok_or_else(|| format!("unknown setting {id}"))?;
        let validated = validate(def, value)?;
        toml_set(&mut self.overrides, id, json_to_toml(&validated))?;
        self.save_overrides()
    }

    pub fn reset(&mut self, id: &str) -> Result<(), String> {
        if id.starts_with("ai.") {
            return Err("Model routing is managed in Mobius, not Helix Settings.".into());
        }
        find_def(id).ok_or_else(|| format!("unknown setting {id}"))?;
        toml_remove(&mut self.overrides, id);
        self.save_overrides()
    }

    /// Re-read `settings.toml` from disk (C++ `ConfigService::forceReload` for overrides).
    pub fn force_reload(&mut self) {
        self.overrides =
            read_toml_file(&self.paths.settings_toml).unwrap_or_else(|| Toml::Table(Default::default()));
    }

    /// Optimistic concurrency for `Configuration1.CommitOverrides` (C++ `commitRemoteOverrides`).
    pub fn commit_remote_overrides(&mut self, baseline: &str, candidate: &str) -> Result<(), String> {
        self.force_reload();
        let baseline_table = toml::from_str::<Toml>(baseline)
            .map_err(|_| "Invalid settings document".to_string())?;
        if baseline_table != self.overrides {
            return Err("Settings changed in another client. Refresh and try again.".into());
        }
        let candidate_table = toml::from_str::<Toml>(candidate)
            .map_err(|_| "Invalid settings document".to_string())?;
        if candidate_table == self.overrides {
            return Ok(());
        }
        self.overrides = candidate_table;
        self.save_overrides()
    }

    fn save_overrides(&self) -> Result<(), String> {
        if let Some(parent) = self.paths.settings_toml.parent() {
            fs::create_dir_all(parent).map_err(|error| error.to_string())?;
        }
        let text = if let Toml::Table(table) = &self.overrides {
            toml::to_string_pretty(table).map_err(|error| error.to_string())?
        } else {
            String::new()
        };
        fs::write(&self.paths.settings_toml, text).map_err(|error| error.to_string())
    }

    pub fn overridden_path(&self, id: &str) -> bool {
        let path = storage_path(id);
        toml_get(&self.overrides, path.as_ref()).is_some()
    }

    pub fn get_json_value(&self, id: &str) -> Option<Value> {
        let path = storage_path(id);
        let toml = toml_get(&self.overrides, path.as_ref())
            .or_else(|| toml_get(&self.base, path.as_ref()))?;
        Some(toml_to_json_value(toml))
    }

    pub fn set_json_value(&mut self, id: &str, value: Value) -> Result<(), String> {
        let path = storage_path(id);
        toml_set(&mut self.overrides, path.as_ref(), json_to_toml(&value))?;
        self.save_overrides()
    }

    pub(crate) fn merged_node(&self, id: &str) -> Option<Toml> {
        let base = toml_get(&self.base, id).cloned();
        let overrides = toml_get(&self.overrides, id).cloned();
        match (base, overrides) {
            (None, None) => None,
            (Some(node), None) => Some(node),
            (None, Some(node)) => Some(node),
            (Some(left), Some(right)) => Some(merge_toml_values(left, right)),
        }
    }

    pub fn get_raw_list(&self, id: &str) -> Option<Value> {
        self.get_json_value(id).and_then(|value| value.as_array().cloned().map(Value::Array))
    }

    pub fn set_raw_list(&mut self, id: &str, value: Value) -> Result<(), String> {
        value
            .as_array()
            .ok_or_else(|| "expected list value".to_string())?;
        self.set_json_value(id, value)
    }

    pub fn apply_ordered_list(&mut self, id: &str, value: Value) -> Result<(), String> {
        super::ordered_lists::apply_update(self, id, value)
    }

    /// Bootstrap catalog plus ordered-list extensions (key-chords, Control Center tabs, …).
    pub fn full_catalog(&self, include_desktop: bool) -> Vec<Value> {
        let mut rows = self.catalog();
        rows.extend(super::ordered_lists::extension_entries(self, include_desktop));
        rows
    }

    /// Catalog rows for `Configuration1.GetSettings`.
    pub fn catalog(&self) -> Vec<Value> {
        BOOTSTRAP
            .iter()
            .map(|def| {
                let mut entry = json!({
                    "id": def.id,
                    "page": def.page,
                    "title": def.title,
                    "group": def.group,
                    "type": def.type_name(),
                    "value": self.get(def.id),
                    "minimum": def.minimum,
                    "maximum": def.maximum,
                    "overridden": self.overridden(def.id),
                });
                if let Some((options, labels)) = def.options() {
                    entry["options"] = Value::Array(options.iter().map(|o| Value::String((*o).into())).collect());
                    entry["option-labels"] =
                        Value::Array(labels.iter().map(|o| Value::String((*o).into())).collect());
                }
                entry
            })
            .collect()
    }
}

/// Whether the session bar should register the native Polkit authentication agent.
pub fn shell_polkit_agent_enabled() -> bool {
    if std::env::var_os("HELIX_DISABLE_POLKIT_AGENT").is_some() {
        return false;
    }
    if std::env::var_os("HELIX_ENABLE_POLKIT_AGENT").is_some() {
        return true;
    }
    ConfigStore::load(ConfigPaths::from_env())
        .get("shell.polkit_agent")
        .as_bool()
        .unwrap_or(false)
}

fn load_merged_config(config_dir: &Path) -> Toml {
    let mut table = toml::map::Map::new();
    let main = config_dir.join("config.toml");
    if let Some(Toml::Table(root)) = read_toml_file(&main) {
        deep_merge(&mut table, root);
    }
    if let Ok(entries) = fs::read_dir(config_dir) {
        let mut files: Vec<PathBuf> = entries
            .filter_map(|entry| entry.ok().map(|e| e.path()))
            .filter(|path| {
                path.extension().and_then(|ext| ext.to_str()) == Some("toml")
                    && path.file_name().and_then(|name| name.to_str()) != Some("config.toml")
            })
            .collect();
        files.sort();
        for path in files {
            if let Some(Toml::Table(part)) = read_toml_file(&path) {
                deep_merge(&mut table, part);
            }
        }
    }
    Toml::Table(table)
}

fn read_toml_file(path: &Path) -> Option<Toml> {
    let text = fs::read_to_string(path).ok()?;
    text.parse().ok()
}

fn deep_merge(into: &mut toml::map::Map<String, Toml>, from: toml::map::Map<String, Toml>) {
    for (key, value) in from {
        match (into.get_mut(&key), value) {
            (Some(Toml::Table(existing)), Toml::Table(incoming)) => deep_merge(existing, incoming),
            (_, value) => {
                into.insert(key, value);
            }
        }
    }
}

/// Maps catalog ids to TOML override paths (C++ `settingPath`).
pub fn storage_path(id: &str) -> Cow<'_, str> {
    if id == "control_center.visible_tabs" {
        return Cow::Borrowed("control_center.hidden_tabs");
    }
    if let Some(rest) = id.strip_prefix("bar:") {
        if let Some((bar_name, field)) = rest.rsplit_once(':') {
            if matches!(field, "start" | "center" | "end") {
                return Cow::Owned(format!("bar.{bar_name}.{field}"));
            }
        }
    }
    Cow::Borrowed(id)
}

fn path_parts(id: &str) -> Vec<&str> {
    id.split('.').collect()
}

fn merge_toml_values(base: Toml, overrides: Toml) -> Toml {
    match (base, overrides) {
        (Toml::Table(mut left), Toml::Table(right)) => {
            deep_merge(&mut left, right);
            Toml::Table(left)
        }
        (_, value) => value,
    }
}

fn toml_get<'a>(root: &'a Toml, id: &str) -> Option<&'a Toml> {
    let mut node = root;
    for part in path_parts(id) {
        node = node.as_table()?.get(part)?;
    }
    Some(node)
}

fn toml_set(root: &mut Toml, id: &str, value: Toml) -> Result<(), String> {
    let parts = path_parts(id);
    if parts.is_empty() {
        return Err("empty setting id".into());
    }
    if !matches!(root, Toml::Table(_)) {
        *root = Toml::Table(Default::default());
    }
    let mut table = root.as_table_mut().unwrap();
    for part in &parts[..parts.len() - 1] {
        table
            .entry((*part).to_string())
            .or_insert_with(|| Toml::Table(Default::default()));
        table = table
            .get_mut(*part)
            .and_then(Toml::as_table_mut)
            .ok_or_else(|| format!("cannot create table for {id}"))?;
    }
    table.insert(parts[parts.len() - 1].to_string(), value);
    Ok(())
}

fn toml_remove(root: &mut Toml, id: &str) {
    let parts = path_parts(id);
    if parts.is_empty() {
        return;
    }
    let Some(table) = root.as_table_mut() else {
        return;
    };
    if parts.len() == 1 {
        table.remove(parts[0]);
        return;
    }
    let mut cursor = table;
    for part in &parts[..parts.len() - 1] {
        let Some(next) = cursor.get_mut(*part).and_then(Toml::as_table_mut) else {
            return;
        };
        cursor = next;
    }
    cursor.remove(parts[parts.len() - 1]);
}

fn toml_to_json_value(value: &Toml) -> Value {
    match value {
        Toml::Boolean(flag) => Value::Bool(*flag),
        Toml::Integer(number) => Value::from(*number),
        Toml::Float(number) => Value::from(*number),
        Toml::String(text) => Value::String(text.clone()),
        Toml::Array(items) => Value::Array(items.iter().map(toml_to_json_value).collect()),
        Toml::Table(table) => {
            let mut object = serde_json::Map::new();
            for (key, item) in table {
                object.insert(key.clone(), toml_to_json_value(item));
            }
            Value::Object(object)
        }
        Toml::Datetime(datetime) => Value::String(datetime.to_string()),
    }
}

fn toml_to_json(value: &Toml, def: &SettingDef) -> Value {
    match def.kind {
        SettingKind::Boolean => Value::Bool(value.as_bool().unwrap_or(false)),
        SettingKind::Number => Value::from(value.as_float().or_else(|| value.as_integer().map(|n| n as f64)).unwrap_or(0.0)),
        SettingKind::Integer => Value::from(value.as_integer().unwrap_or(0) as f64),
        SettingKind::String | SettingKind::Mode | SettingKind::Material => {
            Value::String(value.as_str().unwrap_or("").to_string())
        }
        SettingKind::ApplicationList => {
            let Some(items) = value.as_array() else {
                return Value::Array(Vec::new());
            };
            Value::Array(
                items
                    .iter()
                    .filter_map(|item| item.as_str().map(|text| Value::String(text.to_string())))
                    .collect(),
            )
        }
    }
}

fn json_to_toml(value: &Value) -> Toml {
    match value {
        Value::Bool(flag) => Toml::Boolean(*flag),
        Value::Number(number) => {
            if let Some(integer) = number.as_i64() {
                Toml::Integer(integer)
            } else if let Some(float) = number.as_f64() {
                Toml::Float(float)
            } else {
                Toml::Float(0.0)
            }
        }
        Value::String(text) => Toml::String(text.clone()),
        Value::Array(items) => Toml::Array(items.iter().map(json_to_toml).collect()),
        Value::Object(map) => {
            let mut table = toml::map::Map::new();
            for (key, item) in map {
                table.insert(key.clone(), json_to_toml(item));
            }
            Toml::Table(table)
        }
        Value::Null => Toml::String(String::new()),
    }
}

pub(crate) fn validate_application_list(value: Value) -> Result<Value, String> {
    let items = value.as_array().ok_or("expected application list")?;
    if items.len() > 256 {
        return Err("Expected at most 256 application IDs".into());
    }
    let mut seen = std::collections::BTreeSet::new();
    let mut out = Vec::new();
    for item in items {
        let text = item.as_str().ok_or("Application IDs must be text")?;
        if text.is_empty() || text.len() > 255 || text.contains('/') {
            return Err("Invalid or duplicate list entry".into());
        }
        if text.chars().any(|c| c.is_control()) {
            return Err("Invalid or duplicate list entry".into());
        }
        if !seen.insert(text.to_string()) {
            return Err("Invalid or duplicate list entry".into());
        }
        out.push(Value::String(text.to_string()));
    }
    Ok(Value::Array(out))
}

fn validate(def: &SettingDef, value: Value) -> Result<Value, String> {
    match def.kind {
        SettingKind::Boolean => value
            .as_bool()
            .map(Value::Bool)
            .ok_or_else(|| "expected boolean".into()),
        SettingKind::Number => {
            let number = value.as_f64().ok_or("expected number")?;
            Ok(Value::from(number.clamp(def.minimum, def.maximum)))
        }
        SettingKind::Integer => {
            let number = value.as_f64().ok_or("expected integer")?;
            let clamped = number.round().clamp(def.minimum, def.maximum);
            Ok(Value::from(clamped))
        }
        SettingKind::String => value
            .as_str()
            .map(|text| Value::String(text.to_string()))
            .ok_or_else(|| "expected string".into()),
        SettingKind::Mode | SettingKind::Material => {
            let text = value.as_str().ok_or("expected string")?;
            let (options, _) = def.options().unwrap();
            if options.contains(&text) {
                Ok(Value::String(text.into()))
            } else {
                Err(format!("not an option for {}", def.id))
            }
        }
        SettingKind::ApplicationList => validate_application_list(value),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_paths() -> ConfigPaths {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root = std::env::temp_dir().join(format!("helix-settings-{nanos}"));
        let config_dir = root.join("config");
        let state_dir = root.join("state");
        fs::create_dir_all(&config_dir).unwrap();
        fs::create_dir_all(&state_dir).unwrap();
        fs::write(
            config_dir.join("config.toml"),
            "[audio]\nsound_volume = 0.25\n[theme]\nmode = \"light\"\n",
        )
        .unwrap();
        ConfigPaths {
            config_dir,
            settings_toml: state_dir.join("settings.toml"),
        }
    }

    #[test]
    fn loads_base_and_overrides() {
        let paths = temp_paths();
        let mut store = ConfigStore::load(paths.clone());
        assert_eq!(store.get("audio.sound_volume").as_f64(), Some(0.25));
        assert_eq!(store.get("theme.mode").as_str(), Some("light"));
        store.set("audio.sound_volume", json!(0.8)).unwrap();
        assert!(store.overridden("audio.sound_volume"));
        let reloaded = ConfigStore::load(paths);
        assert_eq!(reloaded.get("audio.sound_volume").as_f64(), Some(0.8));
        assert_eq!(reloaded.get("theme.mode").as_str(), Some("light"));
        let catalog = reloaded.catalog();
        assert!(catalog.iter().any(|row| row["id"] == "theme.mode"));
        assert!(catalog.iter().any(|row| row["id"] == "audio.sound_volume"));
    }

    #[test]
    fn polkit_agent_defaults_on_in_catalog() {
        let paths = temp_paths();
        let mut store = ConfigStore::load(paths);
        assert_eq!(store.get("shell.polkit_agent"), json!(true));
        store
            .set("shell.polkit_agent", json!(false))
            .expect("set polkit");
        assert_eq!(store.get("shell.polkit_agent"), json!(false));
    }

    #[test]
    fn application_lists_round_trip() {
        let paths = temp_paths();
        let mut store = ConfigStore::load(paths.clone());
        assert_eq!(store.get("dock.pinned"), json!([]));
        store
            .set(
                "dock.pinned",
                json!(["org.gnome.Nautilus.desktop", "firefox.desktop"]),
            )
            .unwrap();
        let mut reloaded = ConfigStore::load(paths);
        assert_eq!(
            reloaded.get("dock.pinned"),
            json!(["org.gnome.Nautilus.desktop", "firefox.desktop"])
        );
        assert!(reloaded
            .set("dock.pinned", json!(["bad/id.desktop"]))
            .is_err());
    }

    #[test]
    fn commit_remote_overrides_checks_baseline() {
        let paths = temp_paths();
        let mut store = ConfigStore::load(paths.clone());
        store.set("theme.mode", json!("light")).unwrap();
        let stale = std::fs::read_to_string(&paths.settings_toml).unwrap();
        store.set("theme.mode", json!("dark")).unwrap();
        assert_eq!(
            store.commit_remote_overrides(&stale, "[theme]\nmode = \"light\"\n"),
            Err("Settings changed in another client. Refresh and try again.".into())
        );
        store.force_reload();
        let baseline = std::fs::read_to_string(&paths.settings_toml).unwrap();
        store
            .commit_remote_overrides(&baseline, "[theme]\nmode = \"light\"\n")
            .unwrap();
        assert_eq!(store.get("theme.mode"), json!("light"));
    }
}
