//! What the person can change about notifications, and nothing else: each setting here is one the shell really honours. The file is
//! `~/.config/helix/notification-settings.json`; Settings writes it and the shell watches it. Reading is lenient (a bad or missing
//! value falls back to its default and numbers are clamped) and writing goes through `set`, which only takes the names and ranges
//! listed here. The older `notification_rules` rows are not read by anything and stay out of the page.

use serde::Serialize;
use serde_json::{json, Value};

use crate::catalog::{Change, Control, RowView};

pub const SECONDS_MIN: u32 = 2;
pub const SECONDS_MAX: u32 = 15;
pub const SECONDS_DEFAULT: u32 = 6;
pub const TOASTS_MIN: u32 = 1;
pub const TOASTS_MAX: u32 = 3;
pub const TOASTS_DEFAULT: u32 = 3;

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct NotificationPrefs {
    /// How long a banner stays when the application did not say. An application that asks for a time keeps it.
    pub toast_seconds: u32,
    /// Banners shown at once; the rest wait in the notification list.
    pub max_toasts: u32,
}

impl Default for NotificationPrefs {
    fn default() -> Self {
        NotificationPrefs {
            toast_seconds: SECONDS_DEFAULT,
            max_toasts: TOASTS_DEFAULT,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PrefsError {
    UnknownSetting,
    BadValue,
}

impl NotificationPrefs {
    pub fn parse(text: &str) -> NotificationPrefs {
        let value: Value = serde_json::from_str(text).unwrap_or(Value::Null);
        let mut prefs = NotificationPrefs::default();
        if let Some(n) = value.get("toast_seconds").and_then(Value::as_u64) {
            prefs.toast_seconds = (n.min(1000) as u32).clamp(SECONDS_MIN, SECONDS_MAX);
        }
        if let Some(n) = value.get("max_toasts").and_then(Value::as_u64) {
            prefs.max_toasts = (n.min(1000) as u32).clamp(TOASTS_MIN, TOASTS_MAX);
        }
        prefs
    }

    pub fn to_json(&self) -> String {
        json!({"version": 1, "toast_seconds": self.toast_seconds, "max_toasts": self.max_toasts})
            .to_string()
    }

    /// Changes one setting by name. Numbers outside the range are refused, not quietly clamped, so the page never shows what was not kept.
    pub fn set(&self, name: &str, value: &Value) -> Result<NotificationPrefs, PrefsError> {
        let mut next = self.clone();
        let (slot, min, max) = match name {
            "toast_seconds" => (&mut next.toast_seconds, SECONDS_MIN, SECONDS_MAX),
            "max_toasts" => (&mut next.max_toasts, TOASTS_MIN, TOASTS_MAX),
            _ => return Err(PrefsError::UnknownSetting),
        };
        let n = value.as_u64().ok_or(PrefsError::BadValue)?;
        if !(min as u64..=max as u64).contains(&n) {
            return Err(PrefsError::BadValue);
        }
        *slot = n as u32;
        Ok(next)
    }

    /// The rows the Notifications page draws, in order.
    pub fn rows(&self) -> Vec<RowView> {
        let row = |id: &str, title: &str, description: &str, control: Control| RowView {
            id: id.into(),
            title: title.into(),
            group: "Banners".into(),
            description: description.into(),
            overridden: false,
            control,
        };
        vec![
            row(
                "toast_seconds",
                "Banner time (seconds)",
                "How long a banner stays when the application does not ask for a time. Urgent ones stay until dismissed.",
                Control::Slider { min: SECONDS_MIN as f64, max: SECONDS_MAX as f64, integer: true, value: self.toast_seconds as f64 },
            ),
            row(
                "max_toasts",
                "Banners at once",
                "More than this wait in the notification list.",
                Control::Slider { min: TOASTS_MIN as f64, max: TOASTS_MAX as f64, integer: true, value: self.max_toasts as f64 },
            ),
        ]
    }

    /// A change made on the page, as the page sent it.
    pub fn apply(&self, id: &str, change: &Change) -> Result<NotificationPrefs, PrefsError> {
        match change {
            Change::Number(n) if n.fract() == 0.0 && *n >= 0.0 => self.set(id, &json!(*n as u64)),
            _ => Err(PrefsError::BadValue),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_missing_or_broken_file_means_the_defaults() {
        for text in ["", "{", "[]", "null", "{\"toast_seconds\":\"long\"}"] {
            assert_eq!(
                NotificationPrefs::parse(text),
                NotificationPrefs::default(),
                "{text}"
            );
        }
    }

    #[test]
    fn reading_clamps_and_ignores_what_is_wrong() {
        let p = NotificationPrefs::parse(r#"{"toast_seconds": 9999, "max_toasts": 0}"#);
        assert_eq!((p.toast_seconds, p.max_toasts), (SECONDS_MAX, TOASTS_MIN));
        assert_eq!(
            NotificationPrefs::parse(r#"{"toast_seconds": 0}"#).toast_seconds,
            SECONDS_MIN
        );
    }

    #[test]
    fn writing_is_exact() {
        let p = NotificationPrefs::default();
        assert_eq!(
            p.set("toast_seconds", &json!(10)).unwrap().toast_seconds,
            10
        );
        assert_eq!(p.set("toast_seconds", &json!(1)), Err(PrefsError::BadValue));
        assert_eq!(
            p.set("toast_seconds", &json!(16)),
            Err(PrefsError::BadValue)
        );
        assert_eq!(
            p.set("toast_seconds", &json!(5.5)),
            Err(PrefsError::BadValue)
        );
        assert_eq!(p.set("max_toasts", &json!(4)), Err(PrefsError::BadValue));
        assert_eq!(p.set("max_toasts", &json!("2")), Err(PrefsError::BadValue));
        assert_eq!(
            p.set("position", &json!(1)),
            Err(PrefsError::UnknownSetting)
        );
    }

    #[test]
    fn the_page_shows_every_setting_once_and_applies_what_it_sends() {
        let p = NotificationPrefs::default();
        let ids: Vec<String> = p.rows().into_iter().map(|r| r.id).collect();
        assert_eq!(ids, ["toast_seconds", "max_toasts"]);
        assert_eq!(
            p.apply("max_toasts", &Change::Number(2.0))
                .unwrap()
                .max_toasts,
            2
        );
        assert_eq!(
            p.apply("max_toasts", &Change::Number(2.5)),
            Err(PrefsError::BadValue)
        );
        assert_eq!(
            p.apply("max_toasts", &Change::Switch(true)),
            Err(PrefsError::BadValue)
        );
    }

    #[test]
    fn what_is_written_reads_back() {
        let p = NotificationPrefs {
            toast_seconds: 12,
            max_toasts: 1,
        };
        assert_eq!(NotificationPrefs::parse(&p.to_json()), p);
    }
}
