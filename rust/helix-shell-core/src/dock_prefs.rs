//! What the person can change about the dock, and nothing else: each setting here is one the shell really honours. The file is
//! `~/.config/helix/dock-settings.json` (not `dock.json`, which holds the pinned applications); Settings writes it and the shell watches it. Reading is lenient (a bad or missing value falls back to its
//! default and numbers are clamped) and writing goes through `set`, which only takes the names and ranges listed here.

use serde::Serialize;

use crate::catalog::{Change, Control, RowView};
use serde_json::{json, Value};

pub const ICON_MIN: u32 = 32;
pub const ICON_MAX: u32 = 72;
pub const ICON_DEFAULT: u32 = 52;
pub const MAGNIFY_MIN: f64 = 1.0;
pub const MAGNIFY_MAX: f64 = 2.0;
pub const MAGNIFY_DEFAULT: f64 = 1.5;

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct DockPrefs {
    pub icon_size: u32,
    pub magnify: bool,
    /// How much larger the icon under the pointer gets (1.5 is half as large again).
    pub magnification: f64,
    /// Hidden until the pointer reaches the bottom edge; otherwise always showing (windows can sit behind it).
    pub auto_hide: bool,
    /// The dot under an open application.
    pub indicators: bool,
    /// Applications that are open but not kept in the dock.
    pub running: bool,
}

impl Default for DockPrefs {
    fn default() -> Self {
        DockPrefs { icon_size: ICON_DEFAULT, magnify: true, magnification: MAGNIFY_DEFAULT, auto_hide: true, indicators: true, running: true }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PrefsError {
    UnknownSetting,
    BadValue,
}

impl DockPrefs {
    pub fn parse(text: &str) -> DockPrefs {
        let value: Value = serde_json::from_str(text).unwrap_or(Value::Null);
        let mut prefs = DockPrefs::default();
        if let Some(n) = value.get("icon_size").and_then(Value::as_u64) {
            prefs.icon_size = (n.min(1000) as u32).clamp(ICON_MIN, ICON_MAX);
        }
        if let Some(n) = value.get("magnification").and_then(Value::as_f64).filter(|n| n.is_finite()) {
            prefs.magnification = round2(n.clamp(MAGNIFY_MIN, MAGNIFY_MAX));
        }
        for (key, slot) in [("magnify", &mut prefs.magnify), ("auto_hide", &mut prefs.auto_hide), ("indicators", &mut prefs.indicators), ("running", &mut prefs.running)] {
            if let Some(b) = value.get(key).and_then(Value::as_bool) {
                *slot = b;
            }
        }
        prefs
    }

    pub fn to_json(&self) -> String {
        json!({"version": 1, "icon_size": self.icon_size, "magnify": self.magnify, "magnification": self.magnification,
               "auto_hide": self.auto_hide, "indicators": self.indicators, "running": self.running})
        .to_string()
    }

    /// Changes one setting by name. Numbers outside the range are refused, not quietly clamped, so the page never shows what was not kept.
    pub fn set(&self, name: &str, value: &Value) -> Result<DockPrefs, PrefsError> {
        let mut next = self.clone();
        match name {
            "icon_size" => {
                let n = value.as_u64().ok_or(PrefsError::BadValue)?;
                if !(ICON_MIN as u64..=ICON_MAX as u64).contains(&n) {
                    return Err(PrefsError::BadValue);
                }
                next.icon_size = n as u32;
            }
            "magnification" => {
                let n = value.as_f64().filter(|n| n.is_finite() && (MAGNIFY_MIN..=MAGNIFY_MAX).contains(n)).ok_or(PrefsError::BadValue)?;
                next.magnification = round2(n);
            }
            "magnify" => next.magnify = value.as_bool().ok_or(PrefsError::BadValue)?,
            "auto_hide" => next.auto_hide = value.as_bool().ok_or(PrefsError::BadValue)?,
            "indicators" => next.indicators = value.as_bool().ok_or(PrefsError::BadValue)?,
            "running" => next.running = value.as_bool().ok_or(PrefsError::BadValue)?,
            _ => return Err(PrefsError::UnknownSetting),
        }
        Ok(next)
    }
}

impl DockPrefs {
    /// The rows the Dock page draws, in order.
    pub fn rows(&self) -> Vec<RowView> {
        let row = |id: &str, title: &str, group: &str, description: &str, control: Control| RowView {
            id: id.into(),
            title: title.into(),
            group: group.into(),
            description: description.into(),
            overridden: false,
            control,
        };
        vec![
            row("auto_hide", "Automatically hide", "Dock", "Shows when the pointer reaches the bottom edge. When off, the dock stays visible and windows can sit behind it.", Control::Switch { on: self.auto_hide }),
            row("running", "Show open applications", "Dock", "Applications that are running but not kept in the dock.", Control::Switch { on: self.running }),
            row("indicators", "Show open indicators", "Dock", "A small dot under applications that are open.", Control::Switch { on: self.indicators }),
            row("icon_size", "Icon size", "Size", "", Control::Slider { min: ICON_MIN as f64, max: ICON_MAX as f64, integer: true, value: self.icon_size as f64 }),
            row("magnify", "Magnify icons", "Size", "Icons grow as the pointer moves over them.", Control::Switch { on: self.magnify }),
            row("magnification", "Magnification amount", "Size", "", Control::Slider { min: MAGNIFY_MIN, max: MAGNIFY_MAX, integer: false, value: self.magnification }),
        ]
    }

    /// A change made on the page, as the page sent it.
    pub fn apply(&self, id: &str, change: &Change) -> Result<DockPrefs, PrefsError> {
        let value = match change {
            Change::Switch(on) => json!(on),
            Change::Number(n) if id == "icon_size" => {
                if n.fract() != 0.0 || *n < 0.0 {
                    return Err(PrefsError::BadValue);
                }
                json!(*n as u64)
            }
            Change::Number(n) => json!(n),
            _ => return Err(PrefsError::BadValue),
        };
        self.set(id, &value)
    }
}

fn round2(n: f64) -> f64 {
    (n * 100.0).round() / 100.0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_missing_or_broken_file_means_the_defaults() {
        for text in ["", "{", "[]", "null", "{\"icon_size\":\"big\"}"] {
            assert_eq!(DockPrefs::parse(text), DockPrefs::default(), "{text}");
        }
    }

    #[test]
    fn reading_clamps_and_ignores_what_is_wrong() {
        let p = DockPrefs::parse(r#"{"icon_size": 9999, "magnification": 7.0, "auto_hide": false, "magnify": "yes", "indicators": false}"#);
        assert_eq!((p.icon_size, p.magnification), (ICON_MAX, MAGNIFY_MAX));
        assert!(!p.auto_hide && !p.indicators);
        assert!(p.magnify, "a value of the wrong type keeps the default");
        assert_eq!(DockPrefs::parse(r#"{"icon_size": 3}"#).icon_size, ICON_MIN);
    }

    #[test]
    fn writing_is_exact() {
        let p = DockPrefs::default();
        assert_eq!(p.set("icon_size", &json!(40)).unwrap().icon_size, 40);
        assert_eq!(p.set("icon_size", &json!(31)), Err(PrefsError::BadValue));
        assert_eq!(p.set("icon_size", &json!(73)), Err(PrefsError::BadValue));
        assert_eq!(p.set("icon_size", &json!(40.5)), Err(PrefsError::BadValue));
        assert_eq!(p.set("icon_size", &json!(-1)), Err(PrefsError::BadValue));
        assert_eq!(p.set("magnification", &json!(1.234)).unwrap().magnification, 1.23);
        assert_eq!(p.set("magnification", &json!(2.5)), Err(PrefsError::BadValue));
        assert_eq!(p.set("magnification", &json!("big")), Err(PrefsError::BadValue));
        assert!(!p.set("auto_hide", &json!(false)).unwrap().auto_hide);
        assert_eq!(p.set("auto_hide", &json!(1)), Err(PrefsError::BadValue));
        assert_eq!(p.set("position", &json!("left")), Err(PrefsError::UnknownSetting));
    }

    #[test]
    fn the_page_shows_every_setting_once_and_applies_what_it_sends() {
        let p = DockPrefs::default();
        let ids: Vec<String> = p.rows().into_iter().map(|r| r.id).collect();
        assert_eq!(ids, ["auto_hide", "running", "indicators", "icon_size", "magnify", "magnification"]);
        for id in &ids {
            assert!(p.set(id, &json!(true)).is_ok() || p.set(id, &json!(40)).is_ok() || p.set(id, &json!(1.5)).is_ok(), "{id} is writable");
        }
        assert_eq!(p.apply("icon_size", &Change::Number(40.0)).unwrap().icon_size, 40);
        assert_eq!(p.apply("icon_size", &Change::Number(40.5)), Err(PrefsError::BadValue));
        assert!(!p.apply("auto_hide", &Change::Switch(false)).unwrap().auto_hide);
        assert_eq!(p.apply("auto_hide", &Change::Number(1.0)), Err(PrefsError::BadValue));
        assert_eq!(p.apply("icon_size", &Change::Text("9".into())), Err(PrefsError::BadValue));
    }

    #[test]
    fn what_is_written_reads_back() {
        let p = DockPrefs::default().set("icon_size", &json!(64)).unwrap().set("running", &json!(false)).unwrap().set("magnification", &json!(1.8)).unwrap();
        assert_eq!(DockPrefs::parse(&p.to_json()), p);
    }
}
