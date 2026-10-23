//! Bootstrap Configuration1 catalog: definitions + TOML load/save for the settings the
//! Tauri shell and Settings app need today. Full C++ catalog ports later.

#[path = "settings_bootstrap_extra.rs"]
mod settings_bootstrap_extra;

use std::borrow::Cow;
use std::fs;
use std::path::{Path, PathBuf};

use serde_json::{json, Value};
use toml::Value as Toml;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SettingKind {
    Language,
    Source,
    Boolean,
    Number,
    Integer,
    String,
    Mode,
    Material,
    /// `shell-layer` (notification popups, etc.).
    ShellLayer,
    /// `screen-position` (notification/OSD anchors).
    ScreenPosition,
    /// `orientation` (OSD layout).
    Orientation,
    /// Desktop-entry id list (`dock.pinned`, launcher pins).
    ApplicationList,
    /// Dock edge (`dock.position`).
    Position,
    /// Launcher button placement on the dock (`dock.launcher_position`).
    DockLauncher,
    /// Hot corner trigger (`hot_corners.*.action`).
    HotCornerAction,
    /// Wallpaper image fit (`wallpaper.fill_mode`).
    WallpaperFit,
    WallpaperSource,
    WallpaperOrder,
    DailyProvider,
    VideoPause,
    /// Clipboard auto-paste mode (`shell.clipboard_auto_paste`).
    ClipboardPaste,
    /// Password masking style (`shell.password_style`).
    PasswordMask,
    /// Attached vs floating panel (`shell.panel.*_placement`).
    PanelPlacement,
    /// Floating panel anchor (`shell.panel.*_position`).
    PanelPosition,
    ShellMode,
    WallpaperScheme,
    NiriOverview,
    Color,
    Palette,
    CommunityPalette,
    CustomPalette,
    Directory,
    WeatherUnit,
    ShadowDirection,
    SidebarMode,
    StorageKey,
    WifiBand,
    /// `bar-anchor` — bar names are filled in at catalog build time when config is loaded.
    BarAnchor,
}

/// C++ `validateSettingValue` / Configuration1.Update messaging.
pub const SETTING_UNAVAILABLE: &str = "Setting is unavailable on this device";

/// Minimum separation between day and night Kelvin values (C++ `NightLightConfig::kTemperatureGap`).
const NIGHTLIGHT_TEMPERATURE_GAP: f64 = 100.0;
pub const MOBIUS_ROUTING_MESSAGE: &str =
    "Model routing is managed in Mobius (helix-mobius), not Helix Settings.";

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

/// `i18n::kSupportedLanguages` (code, display name).
pub const SUPPORTED_LANGUAGES: &[(&str, &str)] = &[
    ("ar", "العربية"),
    ("be", "Беларуская"),
    ("be-Latn", "Biełaruskaja (Łacinka)"),
    ("ca", "Català"),
    ("cs", "Čeština"),
    ("de", "Deutsch"),
    ("en", "English"),
    ("es", "Español"),
    ("fr", "Français"),
    ("gl-ES", "Galego"),
    ("he", "עברית"),
    ("hu", "Magyar"),
    ("it", "Italiano"),
    ("ja", "日本語"),
    ("ko", "한국어"),
    ("ku", "Kurdî"),
    ("nl", "Nederlands"),
    ("nn", "Norsk nynorsk"),
    ("pl", "Polski"),
    ("pt-BR", "Português (Brasil)"),
    ("ru", "Русский"),
    ("sv", "Svenska"),
    ("tr", "Türkçe"),
    ("uk-UA", "Українська"),
    ("vi", "Tiếng Việt"),
    ("zh-Hans", "简体中文"),
    ("zh-Hant", "繁體中文"),
];

impl SettingDef {
    pub fn type_name(&self) -> &'static str {
        match self.kind {
            SettingKind::Boolean => "boolean",
            SettingKind::Number => "number",
            SettingKind::Integer => "integer",
            SettingKind::String => "string",
            SettingKind::Language => "language",
            SettingKind::Source => "source",
            SettingKind::Mode => "mode",
            SettingKind::Material => "material",
            SettingKind::ShellLayer => "shell-layer",
            SettingKind::ScreenPosition => "screen-position",
            SettingKind::Orientation => "orientation",
            SettingKind::ApplicationList => "application-list",
            SettingKind::Position => "position",
            SettingKind::DockLauncher => "dock-launcher",
            SettingKind::HotCornerAction => "hot-corner-action",
            SettingKind::WallpaperFit => "wallpaper-fit",
            SettingKind::WallpaperSource => "wallpaper-source",
            SettingKind::WallpaperOrder => "wallpaper-order",
            SettingKind::DailyProvider => "daily-provider",
            SettingKind::VideoPause => "video-pause",
            SettingKind::ClipboardPaste => "clipboard-paste",
            SettingKind::PasswordMask => "password-mask",
            SettingKind::PanelPlacement => "panel-placement",
            SettingKind::PanelPosition => "panel-position",
            SettingKind::ShellMode => "shell-mode",
            SettingKind::WallpaperScheme => "wallpaper-scheme",
            SettingKind::NiriOverview => "niri-overview",
            SettingKind::Color => "color",
            SettingKind::Palette => "palette",
            SettingKind::CommunityPalette => "community",
            SettingKind::CustomPalette => "custom",
            SettingKind::Directory => "directory",
            SettingKind::WeatherUnit => "weather-unit",
            SettingKind::ShadowDirection => "shadow-direction",
            SettingKind::SidebarMode => "sidebar-mode",
            SettingKind::StorageKey => "storage-key",
            SettingKind::WifiBand => "wifi-band",
            SettingKind::BarAnchor => "bar-anchor",
        }
    }

    pub fn options(&self) -> Option<(&'static [&'static str], &'static [&'static str])> {
        match self.kind {
            SettingKind::Mode => {
                Some((&["dark", "light", "auto"], &["Dark", "Light", "Automatic"]))
            }
            SettingKind::Material => {
                Some((&["solid", "soft", "glass"], &["Solid", "Soft", "Glass"]))
            }
            SettingKind::ShellLayer => {
                Some((&["top", "overlay"], &["Normal", "Above fullscreen windows"]))
            }
            SettingKind::ScreenPosition => Some((
                &[
                    "top_left",
                    "top_center",
                    "top_right",
                    "bottom_left",
                    "bottom_center",
                    "bottom_right",
                ],
                &[
                    "Top left",
                    "Top center",
                    "Top right",
                    "Bottom left",
                    "Bottom center",
                    "Bottom right",
                ],
            )),
            SettingKind::Orientation => {
                Some((&["horizontal", "vertical"], &["Horizontal", "Vertical"]))
            }
            SettingKind::Position => Some((
                &["bottom", "left", "right", "top"],
                &["Bottom", "Left", "Right", "Top"],
            )),
            SettingKind::DockLauncher => {
                Some((&["none", "start", "end"], &["None", "Start", "End"]))
            }
            SettingKind::HotCornerAction => Some((
                &[
                    "none",
                    "launcher",
                    "control_center",
                    "window_switcher",
                    "command",
                ],
                &[
                    "None",
                    "Launcher",
                    "Control Center",
                    "Window switcher",
                    "Custom command",
                ],
            )),
            SettingKind::WallpaperFit => Some((
                &["center", "crop", "fit", "stretch", "repeat", "span"],
                &["Center", "Fill", "Fit", "Stretch", "Tile", "Span displays"],
            )),
            SettingKind::WallpaperSource => {
                Some((&["folder", "daily"], &["Folder", "Daily image"]))
            }
            SettingKind::WallpaperOrder => {
                Some((&["random", "alphabetical"], &["Random", "Alphabetical"]))
            }
            SettingKind::DailyProvider => Some((&["bing", "nasa"], &["Bing", "NASA"])),
            SettingKind::VideoPause => Some((
                &["off", "max", "full"],
                &["Never", "Maximized windows", "Fullscreen windows"],
            )),
            SettingKind::ClipboardPaste => Some((
                &["off", "auto", "ctrl_v", "ctrl_shift_v", "shift_insert"],
                &["Off", "Automatic", "Ctrl+V", "Ctrl+Shift+V", "Shift+Insert"],
            )),
            SettingKind::PasswordMask => {
                Some((&["default", "random"], &["Filled circles", "Random icons"]))
            }
            SettingKind::PanelPlacement => {
                Some((&["attached", "floating"], &["Attached to bar", "Floating"]))
            }
            SettingKind::PanelPosition => Some((
                &[
                    "auto",
                    "center",
                    "top_left",
                    "top_center",
                    "top_right",
                    "center_left",
                    "center_right",
                    "bottom_left",
                    "bottom_center",
                    "bottom_right",
                ],
                &[
                    "Automatic",
                    "Center",
                    "Top left",
                    "Top center",
                    "Top right",
                    "Center left",
                    "Center right",
                    "Bottom left",
                    "Bottom center",
                    "Bottom right",
                ],
            )),
            SettingKind::ShellMode => Some((
                &["follow", "dark", "light", "auto"],
                &["Follow appearance", "Dark", "Light", "Automatic"],
            )),
            SettingKind::WallpaperScheme => Some((
                &[
                    "m3-content",
                    "m3-tonal-spot",
                    "m3-fruit-salad",
                    "m3-rainbow",
                    "m3-monochrome",
                    "vibrant",
                    "faithful",
                    "soft",
                    "dysfunctional",
                    "muted",
                ],
                &[
                    "Content",
                    "Tonal spot",
                    "Fruit salad",
                    "Rainbow",
                    "Monochrome",
                    "Vibrant",
                    "Faithful",
                    "Soft",
                    "Dysfunctional",
                    "Muted",
                ],
            )),
            SettingKind::NiriOverview => Some((
                &["none", "backdrop", "stationary", "flat"],
                &["None", "Backdrop", "Stationary wallpaper", "Flat color"],
            )),
            SettingKind::WeatherUnit => Some((&["metric", "imperial"], &["Celsius", "Fahrenheit"])),
            SettingKind::ShadowDirection => Some((
                &[
                    "center",
                    "down",
                    "up",
                    "left",
                    "right",
                    "down_left",
                    "down_right",
                    "up_left",
                    "up_right",
                ],
                &[
                    "Center",
                    "Down",
                    "Up",
                    "Left",
                    "Right",
                    "Down left",
                    "Down right",
                    "Up left",
                    "Up right",
                ],
            )),
            SettingKind::SidebarMode => Some((
                &["full", "compact", "none"],
                &["Full sidebar", "Compact sidebar", "No sidebar"],
            )),
            SettingKind::StorageKey => {
                Some((&["secret-service", "file"], &["Secret Service", "Key file"]))
            }
            SettingKind::WifiBand => Some((
                &["all", "2.4", "5", "6"],
                &["All bands", "2.4 GHz", "5 GHz", "6 GHz"],
            )),
            SettingKind::BarAnchor => Some((&[""], &["Automatic"])),
            SettingKind::Palette => Some((
                super::palette_names::BUILTIN_PALETTE_NAMES,
                super::palette_names::BUILTIN_PALETTE_NAMES,
            )),
            SettingKind::Color => Some((
                &[
                    "",
                    "primary",
                    "on_primary",
                    "secondary",
                    "on_secondary",
                    "tertiary",
                    "on_tertiary",
                    "error",
                    "on_error",
                    "surface",
                    "on_surface",
                    "surface_variant",
                    "on_surface_variant",
                    "outline",
                    "shadow",
                    "hover",
                    "on_hover",
                ],
                &[
                    "Default",
                    "Primary",
                    "On primary",
                    "Secondary",
                    "On secondary",
                    "Tertiary",
                    "On tertiary",
                    "Error",
                    "On error",
                    "Surface",
                    "On surface",
                    "Surface variant",
                    "On surface variant",
                    "Outline",
                    "Shadow",
                    "Hover",
                    "On hover",
                ],
            )),
            SettingKind::Directory | SettingKind::CommunityPalette | SettingKind::CustomPalette => {
                None
            }
            _ => None,
        }
    }
}

fn bootstrap_defs() -> impl Iterator<Item = &'static SettingDef> {
    BOOTSTRAP
        .iter()
        .chain(settings_bootstrap_extra::BOOTSTRAP_EXTRA.iter())
}

/// The accent colours Helix offers, by name. The stylesheet (`core/shared-ui/style.css`, `:root[data-accent=…]`) turns a name into colours.
pub const ACCENTS: [&str; 8] = [
    "blue", "purple", "pink", "red", "orange", "yellow", "green", "graphite",
];

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
        id: "theme.accent",
        page: "appearance",
        title: "Accent colour",
        group: "Colors",
        kind: SettingKind::String,
        desktop: false,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("blue"),
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
        id: "theme.shell_mode",
        page: "appearance",
        title: "Shell chrome appearance",
        group: "Colors",
        kind: SettingKind::ShellMode,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("follow"),
    },
    SettingDef {
        id: "theme.wallpaper_scheme",
        page: "appearance",
        title: "Wallpaper palette scheme",
        group: "Colors",
        kind: SettingKind::WallpaperScheme,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("m3-content"),
    },
    SettingDef {
        id: "theme.templates.enable_builtin_templates",
        page: "appearance",
        title: "Built-in templates",
        group: "Templates",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "theme.templates.enable_community_templates",
        page: "appearance",
        title: "Community templates",
        group: "Templates",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "theme.source",
        page: "appearance",
        title: "Palette source",
        group: "Colors",
        kind: SettingKind::Source,
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
        kind: SettingKind::Palette,
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
        kind: SettingKind::CommunityPalette,
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
        kind: SettingKind::CustomPalette,
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
        id: "wallpaper.fill_mode",
        page: "appearance",
        title: "Image fit",
        group: "Wallpaper",
        kind: SettingKind::WallpaperFit,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("crop"),
    },
    SettingDef {
        id: "wallpaper.transition_duration",
        page: "appearance",
        title: "Transition duration (milliseconds)",
        group: "Wallpaper",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 100.0,
        maximum: 30000.0,
        default: DefaultValue::Number(1500.0),
    },
    SettingDef {
        id: "wallpaper.transition_on_startup",
        page: "appearance",
        title: "Animate wallpaper on startup",
        group: "Wallpaper",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "wallpaper.edge_smoothness",
        page: "appearance",
        title: "Transition edge smoothness",
        group: "Wallpaper",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.0,
        maximum: 1.0,
        default: DefaultValue::Number(0.3),
    },
    SettingDef {
        id: "wallpaper.automation.enabled",
        page: "appearance",
        title: "Automatically change wallpaper",
        group: "Wallpaper Automation",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "wallpaper.automation.interval_seconds",
        page: "appearance",
        title: "Change interval (seconds)",
        group: "Wallpaper Automation",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 1.0,
        maximum: 86400.0,
        default: DefaultValue::Number(1800.0),
    },
    SettingDef {
        id: "wallpaper.automation.source",
        page: "appearance",
        title: "Wallpaper source",
        group: "Wallpaper Automation",
        kind: SettingKind::WallpaperSource,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("folder"),
    },
    SettingDef {
        id: "wallpaper.automation.order",
        page: "appearance",
        title: "Folder order",
        group: "Wallpaper Automation",
        kind: SettingKind::WallpaperOrder,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("random"),
    },
    SettingDef {
        id: "wallpaper.automation.recursive",
        page: "appearance",
        title: "Include subfolders",
        group: "Wallpaper Automation",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "wallpaper.daily.provider",
        page: "appearance",
        title: "Daily image provider",
        group: "Wallpaper Automation",
        kind: SettingKind::DailyProvider,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("bing"),
    },
    SettingDef {
        id: "wallpaper.daily.bing_market",
        page: "appearance",
        title: "Bing market code",
        group: "Wallpaper Automation",
        kind: SettingKind::String,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str(""),
    },
    SettingDef {
        id: "wallpaper.video.mute",
        page: "appearance",
        title: "Mute video audio",
        group: "Video Wallpaper",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "wallpaper.video.hardware_decode",
        page: "appearance",
        title: "Hardware decoding",
        group: "Video Wallpaper",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "wallpaper.video.auto_pause",
        page: "appearance",
        title: "Pause video behind windows",
        group: "Video Wallpaper",
        kind: SettingKind::VideoPause,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("full"),
    },
    SettingDef {
        id: "wallpaper.video.mpv_options",
        page: "appearance",
        title: "Extra mpv options",
        group: "Video Wallpaper",
        kind: SettingKind::String,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str(""),
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
        id: "shell.external_ip_enabled",
        page: "privacy-security",
        title: "Look up public IP address",
        group: "Network Privacy",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "shell.telemetry_enabled",
        page: "privacy-security",
        title: "Anonymous startup ping",
        group: "Network Privacy",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "shell.screen_time_enabled",
        page: "privacy-security",
        title: "Record application screen time",
        group: "Activity Privacy",
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
        id: "shell.clipboard_keep_from_closed_apps",
        page: "privacy-security",
        title: "Keep copied items after apps close",
        group: "Clipboard",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "shell.clipboard_history_max_entries",
        page: "privacy-security",
        title: "History size",
        group: "Clipboard",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 10.0,
        maximum: 10000.0,
        default: DefaultValue::Number(100.0),
    },
    SettingDef {
        id: "shell.clipboard_confirm_clear_history",
        page: "privacy-security",
        title: "Confirm history deletion",
        group: "Clipboard",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "shell.clipboard_auto_paste",
        page: "privacy-security",
        title: "Paste selected items automatically",
        group: "Clipboard",
        kind: SettingKind::ClipboardPaste,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("auto"),
    },
    SettingDef {
        id: "shell.clipboard_image_action_command",
        page: "privacy-security",
        title: "Image editor command",
        group: "Clipboard",
        kind: SettingKind::String,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str(""),
    },
    SettingDef {
        id: "shell.password_style",
        page: "privacy-security",
        title: "Password masking",
        group: "Authentication",
        kind: SettingKind::PasswordMask,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("default"),
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
        id: "lockscreen.fingerprint",
        page: "privacy-security",
        title: "Use fingerprint authentication",
        group: "Authentication",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "lockscreen.allow_empty_password",
        page: "privacy-security",
        title: "Allow unlocking without a password",
        group: "Lock Screen",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "lockscreen.wallpaper",
        page: "privacy-security",
        title: "Lock-screen wallpaper",
        group: "Lock Screen",
        kind: SettingKind::String,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str(""),
    },
    SettingDef {
        id: "lockscreen.blurred_desktop",
        page: "privacy-security",
        title: "Use blurred desktop background",
        group: "Lock Screen",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "lockscreen.blur_intensity",
        page: "privacy-security",
        title: "Background blur",
        group: "Lock Screen",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.0,
        maximum: 1.0,
        default: DefaultValue::Number(0.5),
    },
    SettingDef {
        id: "lockscreen.tint_intensity",
        page: "privacy-security",
        title: "Background tint",
        group: "Lock Screen",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.0,
        maximum: 1.0,
        default: DefaultValue::Number(0.3),
    },
    SettingDef {
        id: "lockscreen.lock_before_suspend",
        page: "power",
        title: "Lock before system suspend",
        group: "Session",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "lockscreen.lid_close_keep_external_displays",
        page: "power",
        title: "Keep external displays active when closing the lid",
        group: "Session",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "location.auto_locate",
        page: "privacy-security",
        title: "Locate automatically",
        group: "Location",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "location.address",
        page: "privacy-security",
        title: "Address",
        group: "Location",
        kind: SettingKind::String,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str(""),
    },
    SettingDef {
        id: "location.latitude",
        page: "privacy-security",
        title: "Latitude",
        group: "Location",
        kind: SettingKind::Number,
        desktop: true,
        minimum: -90.0,
        maximum: 90.0,
        default: DefaultValue::Number(0.0),
    },
    SettingDef {
        id: "location.longitude",
        page: "privacy-security",
        title: "Longitude",
        group: "Location",
        kind: SettingKind::Number,
        desktop: true,
        minimum: -180.0,
        maximum: 180.0,
        default: DefaultValue::Number(0.0),
    },
    SettingDef {
        id: "location.custom_schedule",
        page: "privacy-security",
        title: "Use custom day and night times",
        group: "Location",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "location.sunrise",
        page: "privacy-security",
        title: "Sunrise (HH:MM)",
        group: "Location",
        kind: SettingKind::String,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("06:00"),
    },
    SettingDef {
        id: "location.sunset",
        page: "privacy-security",
        title: "Sunset (HH:MM)",
        group: "Location",
        kind: SettingKind::String,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("18:00"),
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
        id: "notification.show_actions",
        page: "notifications",
        title: "Show notification actions",
        group: "Popups",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "notification.position",
        page: "notifications",
        title: "Popup position",
        group: "Popups",
        kind: SettingKind::ScreenPosition,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("top_right"),
    },
    SettingDef {
        id: "notification.scale",
        page: "notifications",
        title: "Popup scale",
        group: "Popups",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.5,
        maximum: 2.5,
        default: DefaultValue::Number(1.0),
    },
    SettingDef {
        id: "notification.offset_x",
        page: "notifications",
        title: "Horizontal margin",
        group: "Popups",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 200.0,
        default: DefaultValue::Number(0.0),
    },
    SettingDef {
        id: "notification.offset_y",
        page: "notifications",
        title: "Vertical margin",
        group: "Popups",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 200.0,
        default: DefaultValue::Number(0.0),
    },
    SettingDef {
        id: "notification.layer",
        page: "notifications",
        title: "Popup layer",
        group: "Popups",
        kind: SettingKind::ShellLayer,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("top"),
    },
    SettingDef {
        id: "notification.background_opacity",
        page: "notifications",
        title: "Popup opacity",
        group: "Popups",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.0,
        maximum: 1.0,
        default: DefaultValue::Number(1.0),
    },
    SettingDef {
        id: "notification.border",
        page: "notifications",
        title: "Show popup outline",
        group: "Popups",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "notification.collapse_on_dismiss",
        page: "notifications",
        title: "Collapse dismissed popups",
        group: "Popups",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "notification.keep_dismissed_in_history",
        page: "notifications",
        title: "Keep dismissed notifications",
        group: "History",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "notification.history_retention_hours",
        page: "notifications",
        title: "History retention (hours)",
        group: "History",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 8760.0,
        default: DefaultValue::Number(168.0),
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
        id: "hot_corners.delay_ms",
        page: "desktop-dock",
        title: "Hold delay (milliseconds)",
        group: "Hot Corners",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 2000.0,
        default: DefaultValue::Number(0.0),
    },
    SettingDef {
        id: "hot_corners.top_left.action",
        page: "desktop-dock",
        title: "Top left action",
        group: "Hot Corners",
        kind: SettingKind::HotCornerAction,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("none"),
    },
    SettingDef {
        id: "hot_corners.top_left.command",
        page: "desktop-dock",
        title: "Top left command",
        group: "Hot Corner Commands",
        kind: SettingKind::String,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str(""),
    },
    SettingDef {
        id: "hot_corners.top_right.action",
        page: "desktop-dock",
        title: "Top right action",
        group: "Hot Corners",
        kind: SettingKind::HotCornerAction,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("none"),
    },
    SettingDef {
        id: "hot_corners.top_right.command",
        page: "desktop-dock",
        title: "Top right command",
        group: "Hot Corner Commands",
        kind: SettingKind::String,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str(""),
    },
    SettingDef {
        id: "hot_corners.bottom_left.action",
        page: "desktop-dock",
        title: "Bottom left action",
        group: "Hot Corners",
        kind: SettingKind::HotCornerAction,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("none"),
    },
    SettingDef {
        id: "hot_corners.bottom_left.command",
        page: "desktop-dock",
        title: "Bottom left command",
        group: "Hot Corner Commands",
        kind: SettingKind::String,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str(""),
    },
    SettingDef {
        id: "hot_corners.bottom_right.action",
        page: "desktop-dock",
        title: "Bottom right action",
        group: "Hot Corners",
        kind: SettingKind::HotCornerAction,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("none"),
    },
    SettingDef {
        id: "hot_corners.bottom_right.command",
        page: "desktop-dock",
        title: "Bottom right command",
        group: "Hot Corner Commands",
        kind: SettingKind::String,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str(""),
    },
    SettingDef {
        id: "dock.enabled",
        page: "desktop-dock",
        title: "Show dock",
        group: "Dock",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "dock.position",
        page: "desktop-dock",
        title: "Position",
        group: "Dock",
        kind: SettingKind::Position,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("bottom"),
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
        id: "dock.smart_auto_hide",
        page: "desktop-dock",
        title: "Hide behind windows",
        group: "Dock",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "dock.icon_size",
        page: "desktop-dock",
        title: "Icon size",
        group: "Dock",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 16.0,
        maximum: 128.0,
        default: DefaultValue::Number(48.0),
    },
    SettingDef {
        id: "dock.magnification",
        page: "desktop-dock",
        title: "Magnify icons",
        group: "Dock",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "dock.magnification_scale",
        page: "desktop-dock",
        title: "Magnification amount",
        group: "Dock",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 1.0,
        maximum: 2.0,
        default: DefaultValue::Number(1.45),
    },
    SettingDef {
        id: "dock.active_monitor_only",
        page: "desktop-dock",
        title: "Show on active display only",
        group: "Dock",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "dock.show_running",
        page: "desktop-dock",
        title: "Show running applications",
        group: "Dock",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "dock.show_dots",
        page: "desktop-dock",
        title: "Show running indicators",
        group: "Dock",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "dock.show_instance_count",
        page: "desktop-dock",
        title: "Show window count badges",
        group: "Dock",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "dock.reserve_space",
        page: "desktop-dock",
        title: "Reserve space for the dock",
        group: "Dock",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "dock.launcher_position",
        page: "desktop-dock",
        title: "Launcher button",
        group: "Dock",
        kind: SettingKind::DockLauncher,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("start"),
    },
    SettingDef {
        id: "dock.launcher_icon",
        page: "desktop-dock",
        title: "Launcher icon name",
        group: "Dock Launcher",
        kind: SettingKind::String,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("grid-dots"),
    },
    SettingDef {
        id: "dock.launcher_custom_image",
        page: "desktop-dock",
        title: "Custom launcher image",
        group: "Dock Launcher",
        kind: SettingKind::String,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str(""),
    },
    SettingDef {
        id: "dock.launcher_custom_image_colorize",
        page: "desktop-dock",
        title: "Tint custom launcher image",
        group: "Dock Launcher",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "dock.main_axis_padding",
        page: "desktop-dock",
        title: "Padding along the dock",
        group: "Dock Layout",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 100.0,
        default: DefaultValue::Number(16.0),
    },
    SettingDef {
        id: "dock.cross_axis_padding",
        page: "desktop-dock",
        title: "Padding across the dock",
        group: "Dock Layout",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 100.0,
        default: DefaultValue::Number(8.0),
    },
    SettingDef {
        id: "dock.item_spacing",
        page: "desktop-dock",
        title: "Space between icons",
        group: "Dock Layout",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 100.0,
        default: DefaultValue::Number(6.0),
    },
    SettingDef {
        id: "dock.margin_ends",
        page: "desktop-dock",
        title: "Inset from display ends",
        group: "Dock Layout",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 500.0,
        default: DefaultValue::Number(0.0),
    },
    SettingDef {
        id: "dock.margin_edge",
        page: "desktop-dock",
        title: "Distance from display edge",
        group: "Dock Layout",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 100.0,
        default: DefaultValue::Number(10.0),
    },
    SettingDef {
        id: "dock.layer",
        page: "desktop-dock",
        title: "Layer",
        group: "Dock",
        kind: SettingKind::ShellLayer,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("top"),
    },
    SettingDef {
        id: "dock.shadow",
        page: "desktop-dock",
        title: "Show shadow",
        group: "Dock",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "dock.background_opacity",
        page: "desktop-dock",
        title: "Background opacity",
        group: "Dock Effects",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.0,
        maximum: 1.0,
        default: DefaultValue::Number(0.88),
    },
    SettingDef {
        id: "dock.radius",
        page: "desktop-dock",
        title: "Corner radius",
        group: "Dock Shape",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 80.0,
        default: DefaultValue::Number(16.0),
    },
    SettingDef {
        id: "dock.border_width",
        page: "desktop-dock",
        title: "Border width",
        group: "Dock Shape",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.0,
        maximum: 20.0,
        default: DefaultValue::Number(0.0),
    },
    SettingDef {
        id: "dock.radius_top_left",
        page: "desktop-dock",
        title: "Top-left radius",
        group: "Dock Shape",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 80.0,
        default: DefaultValue::Number(16.0),
    },
    SettingDef {
        id: "dock.radius_top_right",
        page: "desktop-dock",
        title: "Top-right radius",
        group: "Dock Shape",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 80.0,
        default: DefaultValue::Number(16.0),
    },
    SettingDef {
        id: "dock.radius_bottom_left",
        page: "desktop-dock",
        title: "Bottom-left radius",
        group: "Dock Shape",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 80.0,
        default: DefaultValue::Number(16.0),
    },
    SettingDef {
        id: "dock.radius_bottom_right",
        page: "desktop-dock",
        title: "Bottom-right radius",
        group: "Dock Shape",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 80.0,
        default: DefaultValue::Number(16.0),
    },
    SettingDef {
        id: "dock.concave_edge_corners",
        page: "desktop-dock",
        title: "Concave edge corners",
        group: "Dock Shape",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "dock.active_scale",
        page: "desktop-dock",
        title: "Active icon scale",
        group: "Dock Focus",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.1,
        maximum: 1.75,
        default: DefaultValue::Number(1.0),
    },
    SettingDef {
        id: "dock.inactive_scale",
        page: "desktop-dock",
        title: "Inactive icon scale",
        group: "Dock Focus",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.1,
        maximum: 1.0,
        default: DefaultValue::Number(0.85),
    },
    SettingDef {
        id: "dock.active_opacity",
        page: "desktop-dock",
        title: "Active icon opacity",
        group: "Dock Focus",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.0,
        maximum: 1.0,
        default: DefaultValue::Number(1.0),
    },
    SettingDef {
        id: "dock.inactive_opacity",
        page: "desktop-dock",
        title: "Inactive icon opacity",
        group: "Dock Focus",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.0,
        maximum: 1.0,
        default: DefaultValue::Number(0.85),
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
        id: "shell.panel.control_center_placement",
        page: "desktop-dock",
        title: "Panel placement",
        group: "Control Center",
        kind: SettingKind::PanelPlacement,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("floating"),
    },
    SettingDef {
        id: "shell.panel.control_center_position",
        page: "desktop-dock",
        title: "Floating position",
        group: "Control Center",
        kind: SettingKind::PanelPosition,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("auto"),
    },
    SettingDef {
        id: "shell.panel.open_near_click_control_center",
        page: "desktop-dock",
        title: "Open near the pointer",
        group: "Control Center",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "shell.panel.launcher_placement",
        page: "desktop-dock",
        title: "Panel placement",
        group: "Launcher",
        kind: SettingKind::PanelPlacement,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("floating"),
    },
    SettingDef {
        id: "shell.panel.launcher_position",
        page: "desktop-dock",
        title: "Floating position",
        group: "Launcher",
        kind: SettingKind::PanelPosition,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("center"),
    },
    SettingDef {
        id: "shell.panel.open_near_click_launcher",
        page: "desktop-dock",
        title: "Open near the pointer",
        group: "Launcher",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "shell.launcher.categories",
        page: "desktop-dock",
        title: "Show application categories",
        group: "Launcher",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "shell.launcher.app_grid",
        page: "desktop-dock",
        title: "Show applications as a grid",
        group: "Launcher",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "shell.launcher.show_app_actions",
        page: "desktop-dock",
        title: "Show application actions",
        group: "Launcher",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "shell.launcher.show_app_origin_indicator",
        page: "desktop-dock",
        title: "Show application source",
        group: "Launcher",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "shell.launcher.show_icons",
        page: "desktop-dock",
        title: "Show application icons",
        group: "Launcher",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "shell.launcher.sort_by_usage",
        page: "desktop-dock",
        title: "Sort by usage",
        group: "Launcher",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "shell.launcher.compact",
        page: "desktop-dock",
        title: "Compact results",
        group: "Launcher",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "shell.launcher.auto_paste",
        page: "desktop-dock",
        title: "Paste after copy actions",
        group: "Launcher",
        kind: SettingKind::ClipboardPaste,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("auto"),
    },
    SettingDef {
        id: "shell.launcher.provider_prefix",
        page: "desktop-dock",
        title: "Provider prefix",
        group: "Launcher",
        kind: SettingKind::String,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("/"),
    },
    SettingDef {
        id: "shell.launcher.fetch_exchange_rates",
        page: "desktop-dock",
        title: "Fetch currency exchange rates",
        group: "Launcher",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "shell.window_switcher.mru",
        page: "desktop-dock",
        title: "Most recently used order",
        group: "Window Switcher",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "shell.session.grid",
        page: "desktop-dock",
        title: "Use a grid layout",
        group: "Panels · Session",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "shell.session.grid_columns",
        page: "desktop-dock",
        title: "Columns",
        group: "Panels · Session",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 1.0,
        maximum: 5.0,
        default: DefaultValue::Number(3.0),
    },
    SettingDef {
        id: "shell.session.show_shortcuts",
        page: "desktop-dock",
        title: "Show keyboard shortcuts",
        group: "Panels · Session",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
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
        id: "osd.position",
        page: "desktop-dock",
        title: "Indicator position",
        group: "On-Screen Indicators",
        kind: SettingKind::ScreenPosition,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("top_right"),
    },
    SettingDef {
        id: "osd.position_vertical",
        page: "desktop-dock",
        title: "Vertical indicator position",
        group: "On-Screen Indicators",
        kind: SettingKind::ScreenPosition,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("top_center"),
    },
    SettingDef {
        id: "osd.orientation",
        page: "desktop-dock",
        title: "Indicator orientation",
        group: "On-Screen Indicators",
        kind: SettingKind::Orientation,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("horizontal"),
    },
    SettingDef {
        id: "osd.background_opacity",
        page: "desktop-dock",
        title: "Background opacity",
        group: "On-Screen Indicators",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.0,
        maximum: 1.0,
        default: DefaultValue::Number(1.0),
    },
    SettingDef {
        id: "osd.border",
        page: "desktop-dock",
        title: "Show outline",
        group: "On-Screen Indicators",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "osd.offset_x",
        page: "desktop-dock",
        title: "Horizontal margin",
        group: "On-Screen Indicators",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 200.0,
        default: DefaultValue::Number(0.0),
    },
    SettingDef {
        id: "osd.offset_y",
        page: "desktop-dock",
        title: "Vertical margin",
        group: "On-Screen Indicators",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 200.0,
        default: DefaultValue::Number(0.0),
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
        id: "osd.kinds.volume_output",
        page: "desktop-dock",
        title: "Output volume",
        group: "Indicator Events",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "osd.kinds.volume_input",
        page: "desktop-dock",
        title: "Microphone volume",
        group: "Indicator Events",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "osd.kinds.keyboard_backlight",
        page: "desktop-dock",
        title: "Keyboard backlight",
        group: "Indicator Events",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "osd.kinds.wifi",
        page: "desktop-dock",
        title: "Wi-Fi",
        group: "Indicator Events",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "osd.kinds.bluetooth",
        page: "desktop-dock",
        title: "Bluetooth",
        group: "Indicator Events",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "osd.kinds.power_profile",
        page: "desktop-dock",
        title: "Power profile",
        group: "Indicator Events",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "osd.kinds.caffeine",
        page: "desktop-dock",
        title: "Keep awake",
        group: "Indicator Events",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "osd.kinds.dnd",
        page: "desktop-dock",
        title: "Do Not Disturb",
        group: "Indicator Events",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "osd.kinds.keyboard_layout",
        page: "desktop-dock",
        title: "Keyboard layout",
        group: "Indicator Events",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "osd.kinds.lock_keys",
        page: "desktop-dock",
        title: "Caps Lock and Num Lock",
        group: "Indicator Events",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "osd.kinds.media",
        page: "desktop-dock",
        title: "Media",
        group: "Indicator Events",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "osd.kinds.privacy",
        page: "desktop-dock",
        title: "Privacy",
        group: "Indicator Events",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
    },
    SettingDef {
        id: "theme.niri_layout.gaps",
        page: "desktop-dock",
        title: "Window gaps",
        group: "Niri Layout",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 64.0,
        default: DefaultValue::Number(8.0),
    },
    SettingDef {
        id: "theme.niri_layout.struts",
        page: "desktop-dock",
        title: "Outer padding",
        group: "Niri Layout",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 64.0,
        default: DefaultValue::Number(16.0),
    },
    SettingDef {
        id: "theme.niri_layout.default_column_width",
        page: "desktop-dock",
        title: "Default column width",
        group: "Niri Layout",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 100.0,
        maximum: 2000.0,
        default: DefaultValue::Number(500.0),
    },
    SettingDef {
        id: "theme.niri_layout.overview",
        page: "desktop-dock",
        title: "Overview style",
        group: "Niri Layout",
        kind: SettingKind::NiriOverview,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Str("none"),
    },
    SettingDef {
        id: "theme.niri_layout.focus_ring",
        page: "desktop-dock",
        title: "Focus ring",
        group: "Niri Chrome",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "theme.niri_layout.border",
        page: "desktop-dock",
        title: "Window border",
        group: "Niri Chrome",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "theme.niri_layout.draw_border_with_background",
        page: "desktop-dock",
        title: "Draw border with background",
        group: "Niri Chrome",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "theme.niri_layout.blur",
        page: "desktop-dock",
        title: "Window blur",
        group: "Niri Chrome",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "theme.niri_layout.blur_passes",
        page: "desktop-dock",
        title: "Blur passes",
        group: "Niri Chrome",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 1.0,
        maximum: 8.0,
        default: DefaultValue::Number(2.0),
    },
    SettingDef {
        id: "theme.niri_layout.blur_offset",
        page: "desktop-dock",
        title: "Blur offset",
        group: "Niri Chrome",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.5,
        maximum: 8.0,
        default: DefaultValue::Number(3.0),
    },
    SettingDef {
        id: "theme.niri_layout.blur_noise",
        page: "desktop-dock",
        title: "Blur noise",
        group: "Niri Chrome",
        kind: SettingKind::Number,
        desktop: true,
        minimum: 0.0,
        maximum: 0.2,
        default: DefaultValue::Number(0.03),
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
        minimum: 0.8,
        maximum: 1.5,
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
        id: "shell.panel.floating_offset",
        page: "appearance",
        title: "Floating panel distance",
        group: "Shell chrome",
        kind: SettingKind::Integer,
        desktop: true,
        minimum: 0.0,
        maximum: 200.0,
        default: DefaultValue::Number(8.0),
    },
    SettingDef {
        id: "shell.disable_mipmaps",
        page: "appearance",
        title: "Disable texture mipmaps",
        group: "Rendering",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(false),
    },
    SettingDef {
        id: "shell.shared_gl_context",
        page: "appearance",
        title: "Share OpenGL context",
        group: "Rendering",
        kind: SettingKind::Boolean,
        desktop: true,
        minimum: 0.0,
        maximum: 0.0,
        default: DefaultValue::Bool(true),
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

/// Integer settings report integral JSON numbers (C++ `is_number_integer`), never `34.0`.
pub(crate) fn as_integer(value: Value) -> Value {
    match value.as_f64() {
        Some(number) if number.fract() == 0.0 && number.abs() < 9.0e15 => {
            Value::from(number as i64)
        }
        _ => value,
    }
}

/// Scalar default as the C++ config reports it (generated from its pristine catalog); the
/// hand-written `SettingDef.default` is only a fallback for ids the golden does not cover.
fn effective_default(def: &SettingDef) -> DefaultValue {
    super::settings_defaults_generated::cpp_default(def.id).unwrap_or(def.default)
}

pub fn find_def(id: &str) -> Option<&'static SettingDef> {
    bootstrap_defs().find(|setting| setting.id == id)
}

/// Settings available without a registered Helix desktop (C++ `isPortableSetting`).
pub fn is_portable_setting(id: &str) -> bool {
    find_def(id).is_some_and(|def| !def.desktop)
}

#[derive(Clone, Debug)]
pub struct ConfigPaths {
    pub config_dir: PathBuf,
    pub settings_toml: PathBuf,
}

impl ConfigPaths {
    pub fn from_env() -> Self {
        let config_dir = named_dir("HELIX_CONFIG_HOME", "XDG_CONFIG_HOME", ".config", "helix");
        let state_dir = named_dir(
            "HELIX_STATE_HOME",
            "XDG_STATE_HOME",
            ".local/state",
            "helix",
        );
        Self {
            config_dir,
            settings_toml: state_dir.join("settings.toml"),
        }
    }
}

fn named_dir(helix_env: &str, xdg_env: &str, home_suffix: &str, leaf: &str) -> PathBuf {
    // Like C++ `FileUtils::resolveNamedDir`: the override names a home, `helix` lives inside it.
    if let Ok(value) = std::env::var(helix_env) {
        if !value.is_empty() {
            return PathBuf::from(value).join(leaf);
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
    /// (file, modified, length) of every config file seen at the last load; see `reload_if_changed`.
    signature: Vec<(PathBuf, Option<std::time::SystemTime>, u64)>,
}

impl ConfigStore {
    pub fn load(paths: ConfigPaths) -> Self {
        let base = load_merged_config(&paths.config_dir);
        let overrides =
            read_toml_file(&paths.settings_toml).unwrap_or_else(|| Toml::Table(Default::default()));
        let signature = config_signature(&paths);
        Self {
            base,
            overrides,
            paths,
            signature,
        }
    }

    /// Picks up external edits of `config.toml`, its includes and `settings.toml` (C++ config
    /// file watch). Returns whether anything was reloaded.
    pub fn reload_if_changed(&mut self) -> bool {
        let signature = config_signature(&self.paths);
        if signature == self.signature {
            return false;
        }
        self.base = load_merged_config(&self.paths.config_dir);
        self.overrides = read_toml_file(&self.paths.settings_toml)
            .unwrap_or_else(|| Toml::Table(Default::default()));
        self.signature = signature;
        true
    }

    pub fn paths(&self) -> &ConfigPaths {
        &self.paths
    }

    /// Merged `config.toml` (+ includes) and `settings.toml` as JSON for theme templates (`config.*`).
    pub fn template_config_table(&self) -> serde_json::Value {
        toml_to_json_value(&self.merged_user_toml())
    }

    /// `config.toml` (+ includes) deep-merged with `settings.toml` (pre-schema export input).
    pub(crate) fn merged_user_toml(&self) -> Toml {
        merge_toml_values(self.base.clone(), self.overrides.clone())
    }

    /// Like [`merged_user_toml`] but applies sidecar migrations, merges, drops `config_version`,
    /// then runs `normalizeLegacyConfig` (C++ `buildEffectiveConfigFromSources` pre-parse merge).
    pub fn merged_for_config_export(&self) -> Toml {
        let overrides =
            super::config_settings_migrate::prepare_settings_sidecar(self.overrides.clone());
        let merged = merge_toml_values(self.base.clone(), overrides);
        if let Toml::Table(mut table) = merged {
            table.remove(super::config_settings_migrate::CONFIG_VERSION_KEY);
            let mut root = Toml::Table(table);
            super::config_settings_migrate::normalize_legacy_config(&mut root);
            root
        } else {
            merged
        }
    }

    pub fn get(&self, id: &str) -> Value {
        let def = match find_def(id) {
            Some(def) => def,
            None => return Value::Null,
        };
        if id.starts_with("ai.") {
            let mirror = crate::inference_routing::settings_mirror();
            return match id {
                "ai.act_model" => Value::String(mirror.act_model),
                "ai.chat_model" => Value::String(mirror.chat_model),
                "ai.lan_endpoint" => Value::String(mirror.lan_endpoint),
                "ai.allow_remote_on_battery_saver" => {
                    Value::Bool(mirror.allow_remote_on_battery_saver)
                }
                _ => effective_default(def).to_json(),
            };
        }
        if let Some(value) = toml_get(&self.overrides, id) {
            return toml_to_json(value, def);
        }
        if let Some(value) = toml_get(&self.base, id) {
            return toml_to_json(value, def);
        }
        effective_default(def).to_json()
    }

    pub fn overridden(&self, id: &str) -> bool {
        if id.starts_with("ai.") {
            return false;
        }
        toml_get(&self.overrides, id).is_some()
    }

    pub fn set(&mut self, id: &str, value: Value) -> Result<(), String> {
        if id.starts_with("ai.") {
            return Err(MOBIUS_ROUTING_MESSAGE.to_string());
        }
        if id == "nightlight.temperature_day" || id == "nightlight.temperature_night" {
            return self.set_nightlight_temperatures(id, value);
        }
        if id.starts_with("bar:") {
            return self.set_bar_field(id, value);
        }
        if id.starts_with("idle:") {
            return self.set_idle_field(id, value);
        }
        let def = find_def(id).ok_or_else(|| SETTING_UNAVAILABLE.to_string())?;
        let validated = validate(def, id, value)?;
        self.validate_dynamic_options(def, id, &validated)?;
        toml_set(&mut self.overrides, id, json_to_toml(&validated))?;
        self.save_overrides()
    }

    fn set_nightlight_temperatures(&mut self, id: &str, value: Value) -> Result<(), String> {
        let def = find_def(id).ok_or_else(|| SETTING_UNAVAILABLE.to_string())?;
        let validated = validate(def, id, value)?;
        let target = validated
            .as_f64()
            .ok_or_else(|| "This setting requires a number".to_string())?
            .round();
        let day = self
            .get("nightlight.temperature_day")
            .as_f64()
            .unwrap_or(6500.0)
            .round();
        let night = self
            .get("nightlight.temperature_night")
            .as_f64()
            .unwrap_or(4000.0)
            .round();
        let (day, night) = if id == "nightlight.temperature_day" {
            let day = target;
            let night = night.min(day - NIGHTLIGHT_TEMPERATURE_GAP);
            (day, night)
        } else {
            let night = target;
            let day = day.max(night + NIGHTLIGHT_TEMPERATURE_GAP);
            (day, night)
        };
        let day_def = find_def("nightlight.temperature_day").expect("catalog");
        let night_def = find_def("nightlight.temperature_night").expect("catalog");
        if day < day_def.minimum || day > day_def.maximum {
            return Err("Setting is outside its supported range".into());
        }
        if night < night_def.minimum || night > night_def.maximum {
            return Err("Setting is outside its supported range".into());
        }
        toml_set(
            &mut self.overrides,
            "nightlight.temperature_day",
            Toml::Integer(day as i64),
        )?;
        toml_set(
            &mut self.overrides,
            "nightlight.temperature_night",
            Toml::Integer(night as i64),
        )?;
        self.save_overrides()
    }

    fn set_bar_field(&mut self, id: &str, value: Value) -> Result<(), String> {
        let rest = id
            .strip_prefix("bar:")
            .ok_or_else(|| SETTING_UNAVAILABLE.to_string())?;
        let (bar_name, field) = rest
            .rsplit_once(':')
            .ok_or_else(|| SETTING_UNAVAILABLE.to_string())?;
        if bar_name.is_empty() || field.is_empty() {
            return Err(SETTING_UNAVAILABLE.into());
        }
        let validated = super::dynamic_desktop_catalog::validate_bar_scalar(field, value)?;
        self.set_json_value(id, validated)?;
        Ok(())
    }

    fn set_idle_field(&mut self, id: &str, value: Value) -> Result<(), String> {
        let rest = id
            .strip_prefix("idle:")
            .ok_or_else(|| SETTING_UNAVAILABLE.to_string())?;
        let (name, field) = rest
            .rsplit_once(':')
            .ok_or_else(|| SETTING_UNAVAILABLE.to_string())?;
        if name.is_empty() || field.is_empty() {
            return Err(SETTING_UNAVAILABLE.into());
        }
        let validated = super::dynamic_desktop_catalog::validate_idle_field(field, value)?;
        let mut behaviors = self
            .merged_node("idle.behavior")
            .and_then(|node| node.as_table().cloned())
            .unwrap_or_default();
        let row = behaviors
            .entry(name.to_string())
            .or_insert_with(|| Toml::Table(Default::default()));
        let table = row
            .as_table_mut()
            .ok_or_else(|| "idle behavior row must be a table".to_string())?;
        table.insert(field.to_string(), json_to_toml(&validated));
        if field == "action" && validated.as_str() == Some("suspend") {
            table.insert("lock_before_suspend".to_string(), Toml::Boolean(false));
        }
        toml_set(&mut self.overrides, "idle.behavior", Toml::Table(behaviors))?;
        self.save_overrides()
    }

    fn validate_dynamic_options(
        &self,
        def: &SettingDef,
        id: &str,
        value: &Value,
    ) -> Result<(), String> {
        let text = value.as_str().unwrap_or("");
        if text.is_empty() {
            return Ok(());
        }
        match def.kind {
            SettingKind::CommunityPalette => {
                let names = super::palette_names::list_community_palette_names(
                    self.paths
                        .settings_toml
                        .parent()
                        .unwrap_or(&self.paths.config_dir),
                );
                let previous = self.get(id).as_str().map(str::to_string);
                if names.iter().any(|name| name == text) || previous.as_deref() == Some(text) {
                    Ok(())
                } else {
                    Err("Unknown setting option".into())
                }
            }
            SettingKind::CustomPalette => {
                let names = super::palette_names::list_custom_palette_names(&self.paths.config_dir);
                let previous = self.get(id).as_str().map(str::to_string);
                if names.iter().any(|name| name == text) || previous.as_deref() == Some(text) {
                    Ok(())
                } else {
                    Err("Unknown setting option".into())
                }
            }
            _ => Ok(()),
        }
    }

    /// C++ `Appearance1.SetWallhavenApiKey` → `wallpaper.wallhaven.api_key` override (not a catalog row).
    pub fn set_wallhaven_api_key(&mut self, key: &str) -> Result<(), String> {
        toml_set(
            &mut self.overrides,
            "wallpaper.wallhaven.api_key",
            Toml::String(key.to_string()),
        )?;
        self.save_overrides()
    }

    pub fn reset(&mut self, id: &str) -> Result<(), String> {
        if id.starts_with("ai.") {
            return Err(MOBIUS_ROUTING_MESSAGE.to_string());
        }
        if id == "idle.behavior" {
            if let Toml::Table(root) = &mut self.overrides {
                if let Some(idle) = root.get_mut("idle").and_then(Toml::as_table_mut) {
                    idle.remove("behavior");
                    if idle.is_empty() {
                        root.remove("idle");
                    }
                }
            }
            return self.save_overrides();
        }
        // Dynamic desktop rows: `bar:<name>:<field>` and `idle:<behavior>:<field>`.
        let dynamic_field_ok = |prefix: &str, known: fn(&str) -> bool| {
            id.strip_prefix(prefix)
                .and_then(|rest| rest.rsplit_once(':'))
                .is_some_and(|(name, field)| !name.is_empty() && known(field))
        };
        if dynamic_field_ok("bar:", super::dynamic_desktop_catalog::is_bar_field)
            || dynamic_field_ok("idle:", super::dynamic_desktop_catalog::is_idle_field)
        {
            toml_remove(&mut self.overrides, storage_path(id).as_ref());
            return self.save_overrides();
        }
        // Ordered-list rows (key chords, tab/shortcut/application lists, bar widgets, ...) are
        // extension rows, not bootstrap defs; resetting them drops their override.
        if find_def(id).is_none()
            && super::ordered_lists::catalog_entry(&self.full_catalog(true, &[]), id).is_some()
        {
            toml_remove(&mut self.overrides, storage_path(id).as_ref());
            if id == "notification.filter" {
                toml_remove(&mut self.overrides, "notification.filter_order");
                toml_remove(&mut self.overrides, "notification.filter_replace");
            }
            return self.save_overrides();
        }
        find_def(id).ok_or_else(|| SETTING_UNAVAILABLE.to_string())?;
        if id == "nightlight.temperature_day" || id == "nightlight.temperature_night" {
            toml_remove(&mut self.overrides, "nightlight.temperature_day");
            toml_remove(&mut self.overrides, "nightlight.temperature_night");
        } else {
            toml_remove(&mut self.overrides, id);
        }
        self.save_overrides()
    }

    /// Re-read `settings.toml` from disk (C++ `ConfigService::forceReload` for overrides).
    pub fn force_reload(&mut self) {
        // Mutations see the latest hand edits of config.toml too (C++ re-reads before writing).
        self.base = load_merged_config(&self.paths.config_dir);
        self.overrides = read_toml_file(&self.paths.settings_toml)
            .unwrap_or_else(|| Toml::Table(Default::default()));
        self.signature = config_signature(&self.paths);
    }

    /// Optimistic concurrency for `Configuration1.CommitOverrides` (C++ `commitRemoteOverrides`).
    pub fn commit_remote_overrides(
        &mut self,
        baseline: &str,
        candidate: &str,
    ) -> Result<(), String> {
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
        self.get_json_value(id)
            .and_then(|value| value.as_array().cloned().map(Value::Array))
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
    pub fn full_catalog(&self, include_desktop: bool, display_names: &[String]) -> Vec<Value> {
        // Desktop-only settings (whole pages) exist only once a shell has registered.
        let state_dir = self
            .paths
            .settings_toml
            .parent()
            .unwrap_or(&self.paths.config_dir)
            .to_path_buf();
        let community = super::palette_names::list_community_palette_names(&state_dir);
        let custom = super::palette_names::list_custom_palette_names(&self.paths.config_dir);
        let mut rows: Vec<Value> = bootstrap_defs()
            .filter(|def| include_desktop || !def.desktop)
            // C++ omits the palette pickers until there is something to pick.
            .filter(|def| match def.kind {
                SettingKind::CommunityPalette => !community.is_empty(),
                SettingKind::CustomPalette => !custom.is_empty(),
                _ => true,
            })
            .map(|def| {
                let mut entry = self.catalog_entry(def);
                if def.kind == SettingKind::Language {
                    let current = entry
                        .get("value")
                        .and_then(Value::as_str)
                        .unwrap_or("")
                        .to_string();
                    let mut options = vec![String::new()];
                    let mut labels = vec!["Automatic".to_string()];
                    for (code, name) in SUPPORTED_LANGUAGES {
                        options.push((*code).to_string());
                        labels.push((*name).to_string());
                    }
                    if !current.is_empty() && !options.contains(&current) {
                        options.push(current.clone());
                        labels.push(current);
                    }
                    entry["options"] = json!(options);
                    entry["option-labels"] = json!(labels);
                }
                if def.kind == SettingKind::Source {
                    let mut options = vec!["builtin"];
                    if include_desktop {
                        options.push("wallpaper");
                    }
                    if !community.is_empty() {
                        options.push("community");
                    }
                    if !custom.is_empty() {
                        options.push("custom");
                    }
                    entry["options"] = json!(options);
                    entry["option-labels"] = json!(options);
                }
                entry
            })
            .collect();
        rows.extend(super::ordered_lists::extension_entries(
            self,
            include_desktop,
            display_names,
        ));
        rows.extend(super::dynamic_desktop_catalog::desktop_extension_entries(
            self,
            include_desktop,
        ));
        rows
    }

    /// Catalog rows for `Configuration1.GetSettings`.
    pub fn catalog(&self) -> Vec<Value> {
        bootstrap_defs()
            .map(|def| self.catalog_entry(def))
            .collect()
    }

    fn catalog_entry(&self, def: &SettingDef) -> Value {
        let mut entry = json!({
            "id": def.id,
            "page": def.page,
            "title": def.title,
            "group": def.group,
            "type": def.type_name(),
            "value": if def.kind == SettingKind::Integer { as_integer(self.get(def.id)) } else { self.get(def.id) },
            "minimum": def.minimum,
            "maximum": def.maximum,
            "overridden": self.overridden(def.id),
        });
        if def.id == "theme.community_palette" {
            let names = super::palette_names::list_community_palette_names(
                self.paths
                    .settings_toml
                    .parent()
                    .unwrap_or(&self.paths.config_dir),
            );
            let current = entry.get("value").and_then(|v| v.as_str()).unwrap_or("");
            let mut options: Vec<Value> = names.iter().map(|n| Value::String(n.clone())).collect();
            let mut labels = options.clone();
            if !current.is_empty() && !names.iter().any(|n| n == current) {
                options.push(Value::String(current.to_string()));
                labels.push(Value::String(current.to_string()));
            }
            entry["options"] = Value::Array(options);
            entry["option-labels"] = Value::Array(labels);
        } else if def.id == "theme.custom_palette" {
            let names = super::palette_names::list_custom_palette_names(&self.paths.config_dir);
            let current = entry.get("value").and_then(|v| v.as_str()).unwrap_or("");
            let mut options: Vec<Value> = names.iter().map(|n| Value::String(n.clone())).collect();
            let mut labels = options.clone();
            if !current.is_empty() && !names.iter().any(|n| n == current) {
                options.push(Value::String(current.to_string()));
                labels.push(Value::String(current.to_string()));
            }
            entry["options"] = Value::Array(options);
            entry["option-labels"] = Value::Array(labels);
        } else if def.kind == SettingKind::BarAnchor {
            let mut options: Vec<Value> = vec![Value::String(String::new())];
            let mut labels: Vec<Value> = vec![Value::String("Automatic".into())];
            for name in super::ordered_catalog::bar_names(self) {
                options.push(Value::String(name.clone()));
                labels.push(Value::String(name));
            }
            let current = entry.get("value").and_then(|v| v.as_str()).unwrap_or("");
            if !current.is_empty() && !options.iter().any(|o| o.as_str() == Some(current)) {
                options.push(Value::String(current.into()));
                labels.push(Value::String(current.into()));
            }
            entry["options"] = Value::Array(options);
            entry["option-labels"] = Value::Array(labels);
        } else if let Some((options, labels)) = def.options() {
            entry["options"] =
                Value::Array(options.iter().map(|o| Value::String((*o).into())).collect());
            entry["option-labels"] =
                Value::Array(labels.iter().map(|o| Value::String((*o).into())).collect());
        }
        entry
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

fn config_signature(paths: &ConfigPaths) -> Vec<(PathBuf, Option<std::time::SystemTime>, u64)> {
    let mut files: Vec<PathBuf> = fs::read_dir(&paths.config_dir)
        .map(|entries| {
            entries
                .filter_map(|entry| entry.ok().map(|e| e.path()))
                .filter(|path| path.extension().and_then(|ext| ext.to_str()) == Some("toml"))
                .collect()
        })
        .unwrap_or_default();
    files.push(paths.settings_toml.clone());
    files.sort();
    files
        .into_iter()
        .map(|path| {
            let meta = fs::metadata(&path).ok();
            let modified = meta.as_ref().and_then(|m| m.modified().ok());
            (path, modified, meta.map_or(0, |m| m.len()))
        })
        .collect()
}

fn load_merged_config(config_dir: &Path) -> Toml {
    Toml::Table(super::config_merge::merge_config_with_includes(config_dir))
}

fn read_toml_file(path: &Path) -> Option<Toml> {
    let text = fs::read_to_string(path).ok()?;
    text.parse().ok()
}

fn deep_merge(into: &mut toml::map::Map<String, Toml>, from: toml::map::Map<String, Toml>) {
    super::config_merge::deep_merge(into, from);
}

/// Maps catalog ids to TOML override paths (C++ `settingPath`).
pub fn storage_path(id: &str) -> Cow<'_, str> {
    if id == "control_center.visible_tabs" {
        return Cow::Borrowed("control_center.hidden_tabs");
    }
    if id == "idle.behavior" {
        return Cow::Borrowed("idle.behavior");
    }
    if let Some(rest) = id.strip_prefix("idle:") {
        if let Some((name, field)) = rest.rsplit_once(':') {
            if matches!(
                field,
                "enabled" | "timeout" | "locked_timeout" | "action" | "lock_before_suspend"
            ) {
                return Cow::Owned(format!("idle.behavior.{name}.{field}"));
            }
        }
    }
    if let Some(rest) = id.strip_prefix("bar:") {
        if let Some((bar_name, field)) = rest.rsplit_once(':') {
            if matches!(
                field,
                "enabled"
                    | "position"
                    | "auto_hide"
                    | "smart_auto_hide"
                    | "reserve_space"
                    | "show_on_workspace_switch"
                    | "shadow"
                    | "contact_shadow"
                    | "thickness"
                    | "layer"
                    | "scale"
                    | "font_scale"
                    | "font_weight"
                    | "font_family"
                    | "panel_overlap"
                    | "margin_ends"
                    | "margin_edge"
                    | "margin_opposite_edge"
                    | "padding"
                    | "background_opacity"
                    | "radius"
                    | "border_width"
                    | "concave_edge_corners"
                    | "widget_spacing"
                    | "start"
                    | "center"
                    | "end"
            ) {
                return Cow::Owned(format!("bar.{bar_name}.{field}"));
            }
        }
    }
    Cow::Borrowed(id)
}

pub(crate) fn merge_toml_values(base: Toml, overrides: Toml) -> Toml {
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
    for part in storage_path(id).split('.') {
        node = node.as_table()?.get(part)?;
    }
    Some(node)
}

fn toml_set(root: &mut Toml, id: &str, value: Toml) -> Result<(), String> {
    let path = storage_path(id);
    let parts: Vec<&str> = path.split('.').collect();
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

/// Removes the override at `id`'s storage path and prunes tables the removal leaves empty
/// (the C++ override store never keeps empty groups).
fn toml_remove(root: &mut Toml, id: &str) {
    fn remove_path(table: &mut toml::map::Map<String, Toml>, parts: &[&str]) {
        match parts {
            [] => {}
            [last] => {
                table.remove(*last);
            }
            [head, rest @ ..] => {
                let emptied = match table.get_mut(*head).and_then(Toml::as_table_mut) {
                    Some(child) => {
                        remove_path(child, rest);
                        child.is_empty()
                    }
                    None => false,
                };
                if emptied {
                    table.remove(*head);
                }
            }
        }
    }
    let path = storage_path(id);
    let parts: Vec<&str> = path.split('.').collect();
    if let Some(table) = root.as_table_mut() {
        remove_path(table, &parts);
    }
}

/// Active GTK icon theme name (GNOME `gsettings`, then KDE `kdeglobals`, else `Adwaita`).
pub fn active_icon_theme_name() -> String {
    if let Ok(output) = std::process::Command::new("gsettings")
        .args(["get", "org.gnome.desktop.interface", "icon-theme"])
        .output()
    {
        if output.status.success() {
            let stdout = String::from_utf8_lossy(&output.stdout);
            let unquoted = stdout.trim().trim_matches('\'').trim();
            if !unquoted.is_empty() {
                return unquoted.to_string();
            }
        }
    }
    if let Ok(home) = std::env::var("HOME") {
        let globals = PathBuf::from(home).join(".config").join("kdeglobals");
        if let Ok(text) = std::fs::read_to_string(globals) {
            let mut in_icons = false;
            for line in text.lines() {
                let line = line.trim();
                if line == "[Icons]" {
                    in_icons = true;
                    continue;
                }
                if line.starts_with('[') {
                    in_icons = false;
                    continue;
                }
                if in_icons {
                    if let Some(theme) = line.strip_prefix("Theme=") {
                        let theme = theme.trim();
                        if !theme.is_empty() {
                            return theme.to_string();
                        }
                    }
                }
            }
        }
    }
    "Adwaita".to_string()
}

/// JSON for theme templates and other TOML-backed APIs (integers stay integers).
pub fn toml_to_json_value(value: &Toml) -> Value {
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
        SettingKind::Number => Value::from(
            value
                .as_float()
                .or_else(|| value.as_integer().map(|n| n as f64))
                .unwrap_or(0.0),
        ),
        SettingKind::Integer => {
            let number = value
                .as_integer()
                .or_else(|| value.as_float().map(|n| n.round() as i64))
                .unwrap_or(0);
            Value::from(number)
        }
        SettingKind::String
        | SettingKind::Language
        | SettingKind::Source
        | SettingKind::Color
        | SettingKind::Directory
        | SettingKind::Palette
        | SettingKind::CommunityPalette
        | SettingKind::CustomPalette
        | SettingKind::BarAnchor
        | SettingKind::Mode
        | SettingKind::Material
        | SettingKind::ShellLayer
        | SettingKind::ScreenPosition
        | SettingKind::Orientation
        | SettingKind::Position
        | SettingKind::DockLauncher
        | SettingKind::HotCornerAction
        | SettingKind::WallpaperFit
        | SettingKind::WallpaperSource
        | SettingKind::WallpaperOrder
        | SettingKind::DailyProvider
        | SettingKind::VideoPause
        | SettingKind::ClipboardPaste
        | SettingKind::PasswordMask
        | SettingKind::PanelPlacement
        | SettingKind::PanelPosition
        | SettingKind::ShellMode
        | SettingKind::WallpaperScheme
        | SettingKind::NiriOverview
        | SettingKind::WeatherUnit
        | SettingKind::ShadowDirection
        | SettingKind::SidebarMode
        | SettingKind::StorageKey
        | SettingKind::WifiBand => Value::String(value.as_str().unwrap_or("").to_string()),
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

pub(crate) fn json_to_toml(value: &Value) -> Toml {
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

fn validate_hook_command(text: &str) -> Result<(), String> {
    if text.len() > 4096 {
        return Err("Commands must be at most 4096 characters".into());
    }
    if text.bytes().any(|c| c == 0) {
        return Err("Commands cannot contain null bytes".into());
    }
    Ok(())
}

fn validate_existing_file_path(text: &str, message: &str) -> Result<(), String> {
    if text.len() > 4096 {
        return Err(message.into());
    }
    if text.is_empty() {
        return Ok(());
    }
    let path = Path::new(text);
    if !path.is_absolute() || !path.is_file() {
        return Err(message.into());
    }
    Ok(())
}

fn validate_existing_directory(text: &str) -> Result<(), String> {
    if text.is_empty() {
        return Ok(());
    }
    let path = Path::new(text);
    if !path.is_absolute() || !path.is_dir() {
        return Err("Choose an existing folder".into());
    }
    Ok(())
}

fn validate_hh_mm(text: &str) -> Result<(), String> {
    if text.is_empty() {
        return Ok(());
    }
    if text.len() != 5 || text.as_bytes()[2] != b':' {
        return Err("Use HH:MM time".into());
    }
    let digits = text
        .chars()
        .filter(|c| c.is_ascii_digit())
        .collect::<String>();
    if digits.len() != 4 {
        return Err("Use HH:MM time".into());
    }
    Ok(())
}

fn validate(def: &SettingDef, id: &str, value: Value) -> Result<Value, String> {
    match def.kind {
        SettingKind::Boolean => value
            .as_bool()
            .map(Value::Bool)
            .ok_or_else(|| "This setting requires a boolean".into()),
        SettingKind::Number => {
            let number = value.as_f64().ok_or("This setting requires a number")?;
            let clamped = number.clamp(def.minimum, def.maximum);
            if clamped != number {
                return Err("Setting is outside its supported range".into());
            }
            Ok(Value::from(clamped))
        }
        SettingKind::Integer => {
            let number = value.as_f64().ok_or("This setting requires a number")?;
            if number.fract() != 0.0 {
                return Err("This setting requires a number".into());
            }
            let clamped = number.round().clamp(def.minimum, def.maximum);
            if clamped != number.round() {
                return Err("Setting is outside its supported range".into());
            }
            Ok(Value::from(clamped as i64))
        }
        SettingKind::Color => {
            let text = value.as_str().ok_or("This setting requires text")?;
            super::color_spec::validate_color_config_string(text)?;
            Ok(Value::String(text.to_string()))
        }
        SettingKind::Directory => {
            let text = value.as_str().ok_or("This setting requires text")?;
            validate_existing_directory(text)?;
            Ok(Value::String(text.to_string()))
        }
        SettingKind::Palette | SettingKind::CommunityPalette | SettingKind::CustomPalette => {
            let text = value.as_str().ok_or("This setting requires text")?;
            if def.kind == SettingKind::Palette {
                let (options, _) = def.options().unwrap();
                if !options.contains(&text) {
                    return Err("Unknown setting option".into());
                }
            }
            Ok(Value::String(text.to_string()))
        }
        SettingKind::Language => {
            let text = value.as_str().ok_or("This setting requires text")?;
            if !text.is_empty() && !SUPPORTED_LANGUAGES.iter().any(|(code, _)| *code == text) {
                return Err("Unknown setting option".into());
            }
            Ok(Value::String(text.to_string()))
        }
        SettingKind::Source => {
            let text = value.as_str().ok_or("This setting requires text")?;
            if !matches!(text, "builtin" | "wallpaper" | "community" | "custom") {
                return Err("Unknown setting option".into());
            }
            Ok(Value::String(text.to_string()))
        }
        SettingKind::String => {
            let text = value.as_str().ok_or("This setting requires text")?;
            if id.starts_with("hooks.") {
                validate_hook_command(text)?;
            } else if id == "location.sunrise" || id == "location.sunset" {
                validate_hh_mm(text)?;
            } else if id == "theme.accent" {
                if !ACCENTS.contains(&text) {
                    return Err("Choose one of the accent colours".into());
                }
            } else if id == "shell.launcher.provider_prefix" {
                if text.len() > 16 {
                    return Err("Provider prefixes must be at most 16 characters".into());
                }
                if text.bytes().any(|c| c < 32 || c == 127) {
                    return Err("Provider prefixes cannot contain control characters".into());
                }
            } else if id == "dock.launcher_icon" {
                if text.is_empty() || text.len() > 64 {
                    return Err("Choose a short icon name".into());
                }
                if text
                    .bytes()
                    .any(|c| c < 32 || c == 127 || c == b'/' || c == b'\\')
                {
                    return Err("Icon names cannot contain path separators".into());
                }
            } else if id == "dock.launcher_custom_image" {
                validate_existing_file_path(text, "Choose an existing image file")?;
            } else if id == "lockscreen.wallpaper" {
                validate_existing_file_path(text, "Choose an existing wallpaper image")?;
            }
            Ok(Value::String(text.to_string()))
        }
        SettingKind::BarAnchor => {
            let text = value.as_str().ok_or("This setting requires text")?;
            Ok(Value::String(text.to_string()))
        }
        SettingKind::Mode
        | SettingKind::Material
        | SettingKind::ShellLayer
        | SettingKind::ScreenPosition
        | SettingKind::Orientation
        | SettingKind::Position
        | SettingKind::DockLauncher
        | SettingKind::HotCornerAction
        | SettingKind::WallpaperFit
        | SettingKind::WallpaperSource
        | SettingKind::WallpaperOrder
        | SettingKind::DailyProvider
        | SettingKind::VideoPause
        | SettingKind::ClipboardPaste
        | SettingKind::PasswordMask
        | SettingKind::PanelPlacement
        | SettingKind::PanelPosition
        | SettingKind::ShellMode
        | SettingKind::WallpaperScheme
        | SettingKind::NiriOverview
        | SettingKind::WeatherUnit
        | SettingKind::ShadowDirection
        | SettingKind::SidebarMode
        | SettingKind::StorageKey
        | SettingKind::WifiBand => {
            let text = value.as_str().ok_or("This setting requires text")?;
            let (options, _) = def.options().unwrap();
            if options.contains(&text) {
                Ok(Value::String(text.into()))
            } else {
                Err("Unknown setting option".into())
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
    fn bootstrap_catalog_meets_desktop_smoke_minimum() {
        let count = bootstrap_defs().count();
        assert!(
            count >= 300,
            "expected at least 300 scalar catalog rows, got {count}"
        );
    }

    #[test]
    fn community_palette_rejects_unknown_option() {
        let paths = temp_paths();
        let mut store = ConfigStore::load(paths);
        assert!(store
            .set("theme.community_palette", json!("not-a-real-palette"))
            .is_err());
    }

    #[test]
    fn mobius_routing_keys_reject_configuration_update() {
        let paths = temp_paths();
        let mut store = ConfigStore::load(paths);
        let err = store.set("ai.chat_model", json!("x")).unwrap_err();
        assert_eq!(err, MOBIUS_ROUTING_MESSAGE);
    }

    #[test]
    fn storage_path_maps_bar_and_hidden_tabs() {
        assert_eq!(
            storage_path("control_center.visible_tabs").as_ref(),
            "control_center.hidden_tabs"
        );
        assert_eq!(storage_path("bar:top:enabled").as_ref(), "bar.top.enabled");
        assert_eq!(storage_path("bar:top:start").as_ref(), "bar.top.start");
    }

    #[test]
    fn nightlight_temperatures_stay_separated() {
        let paths = temp_paths();
        let mut store = ConfigStore::load(paths);
        let original_day = store.get("nightlight.temperature_day");
        let original_night = store.get("nightlight.temperature_night");
        store
            .set("nightlight.temperature_night", json!(9000))
            .unwrap();
        assert_eq!(
            store.get("nightlight.temperature_night").as_f64(),
            Some(9000.0)
        );
        assert!(store.get("nightlight.temperature_day").as_f64().unwrap() >= 9100.0);
        store
            .set("nightlight.temperature_day", json!(2000))
            .unwrap();
        assert_eq!(
            store.get("nightlight.temperature_day").as_f64(),
            Some(2000.0)
        );
        assert!(store.get("nightlight.temperature_night").as_f64().unwrap() <= 1900.0);
        assert!(store
            .set("nightlight.temperature_day", json!(1000))
            .is_err());
        store.reset("nightlight.temperature_night").unwrap();
        assert_eq!(store.get("nightlight.temperature_day"), original_day);
        assert_eq!(store.get("nightlight.temperature_night"), original_night);
    }

    #[test]
    fn hook_command_validation() {
        let paths = temp_paths();
        let mut store = ConfigStore::load(paths);
        store.set("hooks.started", json!("notify-send hi")).unwrap();
        assert!(store.set("hooks.started", json!("\u{0000}")).is_err());
    }

    #[test]
    fn launcher_provider_prefix_validation() {
        let paths = temp_paths();
        let mut store = ConfigStore::load(paths);
        store
            .set("shell.launcher.provider_prefix", json!(">"))
            .unwrap();
        assert_eq!(store.get("shell.launcher.provider_prefix"), json!(">"));
        let too_long = "x".repeat(17);
        assert!(store
            .set("shell.launcher.provider_prefix", json!(too_long))
            .is_err());
        assert!(store
            .set("shell.launcher.provider_prefix", json!("\u{0007}"))
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

    #[test]
    fn wallpaper_scheme_accepts_custom_generators() {
        let paths = temp_paths();
        let mut store = ConfigStore::load(paths);
        for scheme in ["vibrant", "faithful", "soft", "dysfunctional", "muted"] {
            store
                .set("theme.wallpaper_scheme", json!(scheme))
                .unwrap_or_else(|e| panic!("{scheme}: {e}"));
            assert_eq!(store.get("theme.wallpaper_scheme"), json!(scheme));
        }
        assert!(store
            .set("theme.wallpaper_scheme", json!("softened"))
            .is_err());
    }
}
