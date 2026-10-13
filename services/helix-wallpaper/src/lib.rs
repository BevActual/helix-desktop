//! What to paint, and how an image fills a screen. No Wayland here: the geometry and the configuration are plain and tested.

pub mod picker;

use std::collections::HashMap;
use std::path::PathBuf;

/// What one output shows.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Source {
    Image(PathBuf),
    Color([u8; 3]),
}

/// Shown when nothing is configured or an image cannot be read: a quiet dark blue-grey, never a white flash.
pub const FALLBACK: [u8; 3] = [0x1c, 0x1c, 0x22];

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Config {
    pub default: Option<Source>,
    /// By connector name, e.g. "eDP-1".
    pub monitors: HashMap<String, Source>,
}

/// `"color:#rrggbb"` or a path (`~` and `$HOME` allowed).
pub fn parse_source(text: &str, home: &str) -> Option<Source> {
    let text = text.trim();
    if let Some(hex) = text.strip_prefix("color:#") {
        if hex.len() != 6 || !hex.is_ascii() {
            return None;
        }
        let byte = |i: usize| u8::from_str_radix(&hex[i..i + 2], 16).ok();
        return Some(Source::Color([byte(0)?, byte(2)?, byte(4)?]));
    }
    if text.is_empty() {
        return None;
    }
    let expanded = if let Some(rest) = text.strip_prefix("~/") {
        format!("{home}/{rest}")
    } else {
        text.replace("$HOME", home)
    };
    Some(Source::Image(PathBuf::from(expanded)))
}

/// Reads the wallpaper choice from the settings file the shell has always used (`[wallpaper.default]`, `[wallpaper.monitors.<name>]`).
pub fn parse_config(settings: &str, home: &str) -> Config {
    let Ok(root) = settings.parse::<toml::Table>() else {
        return Config::default();
    };
    let Some(wallpaper) = root.get("wallpaper").and_then(toml::Value::as_table) else {
        return Config::default();
    };
    let path_of = |table: &toml::Table| {
        table
            .get("path")
            .and_then(toml::Value::as_str)
            .and_then(|p| parse_source(p, home))
    };
    let mut config = Config {
        default: wallpaper
            .get("default")
            .and_then(toml::Value::as_table)
            .and_then(path_of),
        monitors: HashMap::new(),
    };
    if let Some(monitors) = wallpaper.get("monitors").and_then(toml::Value::as_table) {
        for (name, entry) in monitors {
            if let Some(source) = entry.as_table().and_then(path_of) {
                config.monitors.insert(name.clone(), source);
            }
        }
    }
    config
}

/// Where Settings writes the person's wallpaper choice. It sits on top of the older settings file, so a choice made here wins and the
/// older file is never written by anything but the service that owns it.
pub fn choice_path(home: &str) -> PathBuf {
    if let Some(path) = std::env::var_os("HELIX_WALLPAPER_CHOICE") {
        return PathBuf::from(path);
    }
    let config = std::env::var("XDG_CONFIG_HOME").unwrap_or_else(|_| format!("{home}/.config"));
    PathBuf::from(config).join("helix/wallpaper.toml")
}

/// The text of a choice file that makes `source` the wallpaper on every output.
pub fn choice_to_toml(source: &Source) -> String {
    let path = match source {
        Source::Color([r, g, b]) => format!("color:#{r:02x}{g:02x}{b:02x}"),
        Source::Image(path) => path.to_string_lossy().into_owned(),
    };
    let mut default = toml::Table::new();
    default.insert("path".into(), toml::Value::String(path));
    let mut wallpaper = toml::Table::new();
    wallpaper.insert("default".into(), toml::Value::Table(default));
    let mut root = toml::Table::new();
    root.insert("wallpaper".into(), toml::Value::Table(wallpaper));
    toml::to_string(&root).unwrap_or_default()
}

impl Config {
    /// `self` with everything `over` says on top of it: its default wins (replacing older per-monitor lines), and so does each monitor it names.
    pub fn overlay(mut self, over: Config) -> Config {
        if over.default.is_some() {
            // Choosing a wallpaper in Settings is a choice for every screen: older per-monitor lines must not keep the previous one.
            self.default = over.default;
            self.monitors.clear();
        }
        self.monitors.extend(over.monitors);
        self
    }

    pub fn for_output(&self, name: Option<&str>) -> Source {
        name.and_then(|n| self.monitors.get(n))
            .or(self.default.as_ref())
            .cloned()
            .unwrap_or(Source::Color(FALLBACK))
    }
}

/// The older settings file with the person's choice (written by Settings) on top.
pub fn load_config(home: &str) -> Config {
    let read = |path: PathBuf| {
        std::fs::read_to_string(path)
            .map(|text| parse_config(&text, home))
            .unwrap_or_default()
    };
    read(settings_path(home)).overlay(read(choice_path(home)))
}

pub fn settings_path(home: &str) -> PathBuf {
    if let Some(path) = std::env::var_os("HELIX_WALLPAPER_SETTINGS") {
        return PathBuf::from(path);
    }
    let state = std::env::var("XDG_STATE_HOME").unwrap_or_else(|_| format!("{home}/.local/state"));
    PathBuf::from(state).join("helix/settings.toml")
}

/// The part of an `iw` x `ih` image that, scaled to `ow` x `oh`, covers the whole output without distortion (centred crop).
/// Returns `(x, y, w, h)` in image pixels.
pub fn cover_crop(iw: u32, ih: u32, ow: u32, oh: u32) -> (u32, u32, u32, u32) {
    if iw == 0 || ih == 0 || ow == 0 || oh == 0 {
        return (0, 0, iw, ih);
    }
    // Compare aspect ratios without division: iw/ih against ow/oh.
    let wider = u64::from(iw) * u64::from(oh) > u64::from(ow) * u64::from(ih);
    if wider {
        let w = (u64::from(ih) * u64::from(ow) / u64::from(oh)).max(1) as u32;
        ((iw - w.min(iw)) / 2, 0, w.min(iw), ih)
    } else {
        let h = (u64::from(iw) * u64::from(oh) / u64::from(ow)).max(1) as u32;
        (0, (ih - h.min(ih)) / 2, iw, h.min(ih))
    }
}

/// Pixel size of the buffer for a surface of `logical` pixels at a fractional scale given in 120ths (the protocol's unit).
pub fn buffer_size(logical: u32, scale120: u32) -> u32 {
    ((u64::from(logical) * u64::from(scale120.max(120)) + 60) / 120).max(1) as u32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_choice_round_trips_and_wins_over_the_older_file() {
        for source in [
            Source::Image("/pics/a \"quoted\" b.jpg".into()),
            Source::Image("/pics/ü ñ.png".into()),
            Source::Color([1, 2, 255]),
        ] {
            let text = choice_to_toml(&source);
            assert_eq!(parse_config(&text, "/h").default, Some(source));
        }
        let old = parse_config("[wallpaper.default]\npath=\"/old.jpg\"\n[wallpaper.monitors.A]\npath=\"/a.jpg\"\n[wallpaper.monitors.B]\npath=\"/b.jpg\"", "/h");
        let new = parse_config("[wallpaper.default]\npath=\"/new.jpg\"\n[wallpaper.monitors.B]\npath=\"color:#000000\"", "/h");
        let merged = old.clone().overlay(new);
        assert_eq!(merged.default, Some(Source::Image("/new.jpg".into())));
        assert!(!merged.monitors.contains_key("A"), "a new default replaces older per-monitor lines");
        assert_eq!(merged.monitors["B"], Source::Color([0, 0, 0]), "a monitor the choice names is kept");
        assert_eq!(old.clone().overlay(Config::default()), old, "an empty choice changes nothing");
    }

    #[test]
    fn colours_and_paths_are_told_apart() {
        assert_eq!(
            parse_source("color:#ff8000", "/h"),
            Some(Source::Color([255, 128, 0]))
        );
        assert_eq!(parse_source("color:#xyz", "/h"), None);
        assert_eq!(parse_source("color:#fff", "/h"), None);
        assert_eq!(parse_source("color:#ffffff00", "/h"), None);
        assert_eq!(
            parse_source("~/Pictures/a.jpg", "/home/me"),
            Some(Source::Image("/home/me/Pictures/a.jpg".into()))
        );
        assert_eq!(
            parse_source("$HOME/a.png", "/home/me"),
            Some(Source::Image("/home/me/a.png".into()))
        );
        assert_eq!(
            parse_source("/abs/b.jpg", "/home/me"),
            Some(Source::Image("/abs/b.jpg".into()))
        );
        assert_eq!(parse_source("  ", "/h"), None);
    }

    const SETTINGS: &str = r#"
[lockscreen]
wallpaper = "/ignored.jpg"
[wallpaper]
directory = "/x"
    [wallpaper.default]
    path = "/pics/main.jpg"
    [wallpaper.last]
    path = "/pics/last.jpg"
    [wallpaper.monitors.HDMI-A-1]
    path = "color:#000000"
    [wallpaper.monitors.eDP-1]
    path = "/pics/laptop.jpg"
    [wallpaper.monitors.broken]
    path = "color:#12"
"#;

    #[test]
    fn the_settings_file_is_read_the_way_the_shell_wrote_it() {
        let config = parse_config(SETTINGS, "/h");
        assert_eq!(config.default, Some(Source::Image("/pics/main.jpg".into())));
        assert_eq!(config.monitors.len(), 2, "the unreadable entry is skipped");
        assert_eq!(
            config.for_output(Some("HDMI-A-1")),
            Source::Color([0, 0, 0])
        );
        assert_eq!(
            config.for_output(Some("eDP-1")),
            Source::Image("/pics/laptop.jpg".into())
        );
        assert_eq!(
            config.for_output(Some("DP-3")),
            Source::Image("/pics/main.jpg".into())
        );
        assert_eq!(
            config.for_output(None),
            Source::Image("/pics/main.jpg".into())
        );
    }

    #[test]
    fn nothing_configured_is_a_quiet_colour_not_a_crash() {
        assert_eq!(parse_config("not toml [[", "/h"), Config::default());
        assert_eq!(parse_config("[other]\nx = 1", "/h"), Config::default());
        assert_eq!(
            Config::default().for_output(Some("eDP-1")),
            Source::Color(FALLBACK)
        );
    }

    #[test]
    fn a_wider_image_loses_its_sides_and_a_taller_one_its_top_and_bottom() {
        assert_eq!(cover_crop(3000, 1000, 1000, 1000), (1000, 0, 1000, 1000));
        assert_eq!(cover_crop(1000, 3000, 1000, 1000), (0, 1000, 1000, 1000));
        assert_eq!(cover_crop(1000, 1000, 1000, 1000), (0, 0, 1000, 1000));
    }

    #[test]
    fn the_crop_has_the_outputs_aspect_ratio_and_stays_inside_the_image() {
        for (iw, ih, ow, oh) in [
            (3840, 2160, 2560, 1600),
            (1920, 1080, 2560, 1600),
            (5000, 3000, 1920, 1080),
            (800, 1200, 2560, 1600),
            (7, 5, 100, 3),
        ] {
            let (x, y, w, h) = cover_crop(iw, ih, ow, oh);
            assert!(
                x + w <= iw && y + h <= ih && w > 0 && h > 0,
                "{iw}x{ih} -> {x},{y},{w},{h}"
            );
            let (a, b) = (u64::from(w) * u64::from(oh), u64::from(h) * u64::from(ow));
            assert!(
                a.abs_diff(b) <= u64::from(ow.max(oh)) * 2,
                "aspect {w}x{h} vs {ow}x{oh}"
            );
        }
    }

    #[test]
    fn degenerate_sizes_do_not_panic() {
        assert_eq!(cover_crop(0, 10, 10, 10), (0, 0, 0, 10));
        assert_eq!(cover_crop(10, 10, 0, 10), (0, 0, 10, 10));
        assert_eq!(cover_crop(10, 10, 10, 0), (0, 0, 10, 10));
        assert_eq!(cover_crop(10, 0, 10, 10), (0, 0, 10, 0));
    }

    #[test]
    fn buffers_are_sized_for_the_fractional_scale_and_rounded() {
        assert_eq!(buffer_size(1707, 180), 2561); // 1707 logical at 1.5
        assert_eq!(buffer_size(1000, 120), 1000);
        assert_eq!(buffer_size(1000, 240), 2000);
        assert_eq!(buffer_size(1000, 0), 1000, "no scale reported means 1.0");
        assert_eq!(buffer_size(0, 120), 1);
    }
}
