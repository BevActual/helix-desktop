//! How the desktop's surfaces are shaped: the corner roundness of each kind of thing, chosen separately, and how clear the glass is. One file,
//! `~/.config/helix/shape-settings.json`, that Settings writes and the shell and the Helix applications watch. Window corners are in pixels and are applied
//! by the compositor (the same numbers also live in its own overrides); everything else is a scale where 1 is as designed and 0 is square.

use serde_json::{json, Value};

pub const WINDOW_DEFAULT: u16 = 12;
pub const WINDOW_MAX: u16 = 32;
pub const SCALE_MAX: f64 = 2.0;
pub const GLASS_DEFAULT: f64 = 0.6;

#[derive(Clone, Debug, PartialEq)]
pub struct Shape {
    /// Every window's corner radius in pixels.
    pub windows: u16,
    /// The corner radius of Helix applications' windows when it differs from every other window's (`None`: the same).
    pub helix_windows: Option<u16>,
    /// The top bar and its buttons.
    pub bar: f64,
    /// The dock, its icons and its menu.
    pub dock: f64,
    /// The panels the shell opens (launcher, control centre, calendar, Mobius panel) and what is inside them.
    pub panels: f64,
    /// Notification banners and the notification list.
    pub toasts: f64,
    /// Everything inside Helix applications: cards, buttons, fields, sheets.
    pub apps: f64,
    /// How clear the glass is: 0 clear, 1 tinted.
    pub glass: f64,
}

impl Default for Shape {
    fn default() -> Self {
        Self {
            windows: WINDOW_DEFAULT,
            helix_windows: None,
            bar: 1.0,
            dock: 1.0,
            panels: 1.0,
            toasts: 1.0,
            apps: 1.0,
            glass: GLASS_DEFAULT,
        }
    }
}

fn scale_of(value: &Value, key: &str) -> Option<f64> {
    value
        .get(key)
        .and_then(Value::as_f64)
        .filter(|s| s.is_finite())
        .map(|s| s.clamp(0.0, SCALE_MAX))
}

impl Shape {
    /// From the file's text. Lenient: anything missing or odd is the default, numbers are clamped. A file from before each thing had its own setting
    /// (`step` and a single `scale`) still reads: the scale goes to every shell part and the step's radius to the windows.
    pub fn parse(text: &str) -> Shape {
        let value: Value = serde_json::from_str(text).unwrap_or(Value::Null);
        let mut shape = Shape::default();
        let legacy = scale_of(&value, "scale");
        if let Some(scale) = legacy {
            shape.bar = scale;
            shape.dock = scale;
            shape.panels = scale;
            shape.toasts = scale;
            shape.apps = scale;
        }
        if let Some(radius) = value
            .get("step")
            .and_then(Value::as_str)
            .and_then(legacy_step_radius)
        {
            shape.windows = radius;
        }
        if let Some(n) = value.get("windows").and_then(Value::as_u64) {
            shape.windows = n.min(u64::from(WINDOW_MAX)) as u16;
        }
        if let Some(n) = value.get("helix_windows").and_then(Value::as_u64) {
            shape.helix_windows = Some(n.min(u64::from(WINDOW_MAX)) as u16);
        }
        for (key, slot) in [
            ("bar", &mut shape.bar),
            ("dock", &mut shape.dock),
            ("panels", &mut shape.panels),
            ("toasts", &mut shape.toasts),
            ("apps", &mut shape.apps),
        ] {
            if let Some(s) = scale_of(&value, key) {
                *slot = s;
            }
        }
        if let Some(g) = value
            .get("glass")
            .and_then(Value::as_f64)
            .filter(|g| g.is_finite())
        {
            shape.glass = g.clamp(0.0, 1.0);
        }
        shape
    }

    pub fn to_json(&self) -> String {
        json!({
            "version": 2,
            "windows": self.windows,
            "helix_windows": self.helix_windows,
            "bar": self.bar, "dock": self.dock, "panels": self.panels, "toasts": self.toasts, "apps": self.apps,
            "glass": self.glass,
        })
        .to_string()
    }

    /// The radius Helix applications' windows get.
    pub fn helix_windows_effective(&self) -> u16 {
        self.helix_windows.unwrap_or(self.windows)
    }

    /// The CSS custom properties a page of the shell or of an application is given: `name: value` pairs for the root element.
    /// `scope` is `"shell"` (the bar, dock, panels, toasts, and the panels' contents) or `"app"`.
    pub fn css_vars(&self, scope: &str) -> Vec<(&'static str, String)> {
        let n = |v: f64| format!("{:.3}", v.clamp(0.0, SCALE_MAX));
        match scope {
            "app" => vec![
                ("--shape-ui", n(self.apps)),
                ("--glass-t", format!("{:.3}", self.glass)),
            ],
            _ => vec![
                ("--shape-bar", n(self.bar)),
                ("--shape-dock", n(self.dock)),
                ("--shape-toasts", n(self.toasts)),
                ("--shape-ui", n(self.panels)),
                ("--glass-t", format!("{:.3}", self.glass)),
            ],
        }
    }

    /// The script that sets those properties on a page's root element. Only numbers go in it.
    pub fn script(&self, scope: &str) -> String {
        self.css_vars(scope)
            .iter()
            .map(|(k, v)| format!("document.documentElement.style.setProperty('{k}','{v}');"))
            .collect::<Vec<_>>()
            .join(" ")
    }
}

fn legacy_step_radius(step: &str) -> Option<u16> {
    match step {
        "square" => Some(0),
        "soft" => Some(6),
        "default" => Some(12),
        "round" => Some(20),
        _ => None,
    }
}

/// `~/.config/helix/shape-settings.json`.
pub fn path() -> Option<std::path::PathBuf> {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(std::path::PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME").map(|h| std::path::PathBuf::from(h).join(".config"))
        })?;
    Some(base.join("helix/shape-settings.json"))
}

pub fn read() -> Shape {
    path()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .map(|t| Shape::parse(&t))
        .unwrap_or_default()
}

/// Writes the file where everything reads it (private to the person; replaced whole so a reader never sees half of it).
pub fn write(shape: &Shape) -> Result<(), String> {
    use std::io::Write;
    use std::os::unix::fs::OpenOptionsExt;
    let path = path().ok_or("no folder for the settings")?;
    std::fs::create_dir_all(path.parent().ok_or("no folder")?).map_err(|e| e.to_string())?;
    let temporary = path.with_extension("tmp");
    {
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(&temporary)
            .map_err(|e| e.to_string())?;
        file.write_all(shape.to_json().as_bytes())
            .map_err(|e| e.to_string())?;
    }
    std::fs::rename(&temporary, &path).map_err(|e| e.to_string())
}

/// Watches the file (a stat once a second) and calls `on_change` with what it says now, whenever it changes. Runs on its own thread.
pub fn watch(on_change: impl Fn(Shape) + Send + 'static) {
    std::thread::spawn(move || {
        let stamp = || {
            path()
                .and_then(|p| std::fs::metadata(p).ok())
                .and_then(|m| m.modified().ok())
        };
        let mut seen = stamp();
        loop {
            std::thread::sleep(std::time::Duration::from_secs(1));
            let now = stamp();
            if now != seen {
                seen = now;
                on_change(read());
            }
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn each_thing_has_its_own_setting_and_it_round_trips() {
        let shape = Shape {
            windows: 4,
            helix_windows: Some(18),
            bar: 0.0,
            dock: 1.5,
            panels: 0.5,
            toasts: 2.0,
            apps: 0.25,
            glass: 0.2,
        };
        assert_eq!(Shape::parse(&shape.to_json()), shape);
        assert_eq!(Shape::default().helix_windows_effective(), WINDOW_DEFAULT);
        assert_eq!(shape.helix_windows_effective(), 18);
    }

    #[test]
    fn a_missing_or_odd_file_is_the_default_and_numbers_are_kept_in_range() {
        for text in ["", "{", "[]", r#"{"bar":"x"}"#, r#"{"windows":-3}"#] {
            assert_eq!(Shape::parse(text), Shape::default(), "{text}");
        }
        let wild = Shape::parse(
            r#"{"windows": 900, "bar": 9, "dock": -1, "glass": 5, "helix_windows": 99}"#,
        );
        assert_eq!(
            (
                wild.windows,
                wild.bar,
                wild.dock,
                wild.glass,
                wild.helix_windows
            ),
            (32, 2.0, 0.0, 1.0, Some(32))
        );
    }

    #[test]
    fn the_older_single_choice_still_reads() {
        let old = Shape::parse(r#"{"version":1,"step":"soft","scale":0.5,"glass":0.3}"#);
        assert_eq!(
            (
                old.windows,
                old.bar,
                old.dock,
                old.panels,
                old.toasts,
                old.apps,
                old.glass
            ),
            (6, 0.5, 0.5, 0.5, 0.5, 0.5, 0.3)
        );
    }

    #[test]
    fn a_page_is_given_only_numbers_and_the_scope_picks_which() {
        let shape = Shape {
            bar: 0.0,
            panels: 0.5,
            apps: 1.5,
            ..Shape::default()
        };
        let shell = shape.css_vars("shell");
        assert!(shell
            .iter()
            .any(|(k, v)| *k == "--shape-bar" && v == "0.000"));
        assert!(
            shell
                .iter()
                .any(|(k, v)| *k == "--shape-ui" && v == "0.500"),
            "the shell's own panels use the panels scale"
        );
        let app = shape.css_vars("app");
        assert!(
            app.iter().any(|(k, v)| *k == "--shape-ui" && v == "1.500"),
            "an application uses the apps scale"
        );
        assert!(!app.iter().any(|(k, _)| *k == "--shape-bar"));
        assert!(shape
            .script("app")
            .chars()
            .all(|c| c.is_ascii_graphic() || c == ' '));
    }
}
