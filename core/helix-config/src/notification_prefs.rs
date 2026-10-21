//! What the person can change about notifications, and nothing else: each setting here is one the shell really honours. The file is
//! `~/.config/helix/notification-settings.json`; Settings writes it and the shell watches it. Reading is lenient (a bad or missing
//! value falls back to its default and numbers are clamped) and writing goes through `set`, which only takes the names and ranges
//! listed here. Quiet hours are a window on the 24-hour clock; the shell decides each hour whether that window is open. The older
//! `notification_rules` rows are not read by anything and stay out of the page.

use serde::Serialize;
use serde_json::{json, Value};

use crate::catalog::{Change, Control, RowView};

pub const SECONDS_MIN: u32 = 2;
pub const SECONDS_MAX: u32 = 15;
pub const SECONDS_DEFAULT: u32 = 6;
pub const TOASTS_MIN: u32 = 1;
pub const TOASTS_MAX: u32 = 3;
pub const TOASTS_DEFAULT: u32 = 3;
pub const QUIET_START_DEFAULT: u32 = 22;
pub const QUIET_END_DEFAULT: u32 = 7;

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct NotificationPrefs {
    /// How long a banner stays when the application did not say. An application that asks for a time keeps it.
    pub toast_seconds: u32,
    /// Banners shown at once; the rest wait in the notification list.
    pub max_toasts: u32,
    /// Hides ordinary banners. Urgent ones still show. The same switch as Quick Settings.
    pub dnd: bool,
    /// When on, ordinary banners stay hidden between `quiet_start` and `quiet_end`.
    pub quiet: bool,
    /// Hour on the 24-hour clock when quiet hours begin (0–23).
    pub quiet_start: u32,
    /// Hour on the 24-hour clock when quiet hours end (0–23). The window ends at the start of this hour.
    pub quiet_end: u32,
}

impl Default for NotificationPrefs {
    fn default() -> Self {
        NotificationPrefs {
            toast_seconds: SECONDS_DEFAULT,
            max_toasts: TOASTS_DEFAULT,
            dnd: false,
            quiet: false,
            quiet_start: QUIET_START_DEFAULT,
            quiet_end: QUIET_END_DEFAULT,
        }
    }
}

/// Whether `hour` (0–23) sits in `[start, end)`. The same hour is an empty window. A later start runs through midnight.
pub fn hour_in_window(start: u32, end: u32, hour: u32) -> bool {
    let start = start % 24;
    let end = end % 24;
    let hour = hour % 24;
    if start == end {
        false
    } else if start < end {
        hour >= start && hour < end
    } else {
        hour >= start || hour < end
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
        if let Some(on) = value.get("dnd").and_then(Value::as_bool) {
            prefs.dnd = on;
        }
        if let Some(on) = value.get("quiet").and_then(Value::as_bool) {
            prefs.quiet = on;
        }
        if let Some(n) = value.get("quiet_start").and_then(Value::as_u64) {
            prefs.quiet_start = (n % 24) as u32;
        }
        if let Some(n) = value.get("quiet_end").and_then(Value::as_u64) {
            prefs.quiet_end = (n % 24) as u32;
        }
        prefs
    }

    /// Quiet hours are on, and `hour` (0–23) is inside the window.
    pub fn quiet_at(&self, hour: u32) -> bool {
        self.quiet && hour_in_window(self.quiet_start, self.quiet_end, hour)
    }

    pub fn to_json(&self) -> String {
        json!({
            "version": 1,
            "toast_seconds": self.toast_seconds,
            "max_toasts": self.max_toasts,
            "dnd": self.dnd,
            "quiet": self.quiet,
            "quiet_start": self.quiet_start,
            "quiet_end": self.quiet_end,
        })
        .to_string()
    }

    /// Changes one setting by name. Numbers outside the range are refused, not quietly clamped, so the page never shows what was not kept.
    pub fn set(&self, name: &str, value: &Value) -> Result<NotificationPrefs, PrefsError> {
        let mut next = self.clone();
        match name {
            "dnd" => next.dnd = value.as_bool().ok_or(PrefsError::BadValue)?,
            "quiet" => next.quiet = value.as_bool().ok_or(PrefsError::BadValue)?,
            "quiet_start" | "quiet_end" => {
                let n = value.as_u64().ok_or(PrefsError::BadValue)?;
                if n > 23 {
                    return Err(PrefsError::BadValue);
                }
                if name == "quiet_start" {
                    next.quiet_start = n as u32;
                } else {
                    next.quiet_end = n as u32;
                }
            }
            "toast_seconds" | "max_toasts" => {
                let (slot, min, max) = match name {
                    "toast_seconds" => (&mut next.toast_seconds, SECONDS_MIN, SECONDS_MAX),
                    _ => (&mut next.max_toasts, TOASTS_MIN, TOASTS_MAX),
                };
                let n = value.as_u64().ok_or(PrefsError::BadValue)?;
                if !(min as u64..=max as u64).contains(&n) {
                    return Err(PrefsError::BadValue);
                }
                *slot = n as u32;
            }
            _ => return Err(PrefsError::UnknownSetting),
        }
        Ok(next)
    }

    /// The rows the Notifications page draws, in order.
    pub fn rows(&self) -> Vec<RowView> {
        let row =
            |group: &str, id: &str, title: &str, description: &str, control: Control| RowView {
                id: id.into(),
                title: title.into(),
                group: group.into(),
                description: description.into(),
                overridden: false,
                control,
            };
        vec![
            row(
                "Quiet",
                "dnd",
                "Do not disturb",
                "Hides banners. Urgent ones still show. This is the same switch as in Quick Settings, and it stays on after a restart.",
                Control::Switch { on: self.dnd },
            ),
            row(
                "Quiet",
                "quiet",
                "Quiet hours",
                "Hides banners between the hours below. Urgent ones still show. Do not disturb stays its own switch.",
                Control::Switch { on: self.quiet },
            ),
            row(
                "Quiet",
                "quiet_start",
                "From (hour)",
                "When quiet hours begin, on the 24-hour clock.",
                Control::Slider { min: 0.0, max: 23.0, integer: true, value: self.quiet_start as f64 },
            ),
            row(
                "Quiet",
                "quiet_end",
                "Until (hour)",
                "Quiet hours end at the start of this hour. If From is later, they run through midnight. The same hour means none.",
                Control::Slider { min: 0.0, max: 23.0, integer: true, value: self.quiet_end as f64 },
            ),
            row(
                "Banners",
                "toast_seconds",
                "Banner time (seconds)",
                "How long a banner stays when the application does not ask for a time. Urgent ones stay until dismissed.",
                Control::Slider { min: SECONDS_MIN as f64, max: SECONDS_MAX as f64, integer: true, value: self.toast_seconds as f64 },
            ),
            row(
                "Banners",
                "max_toasts",
                "Banners at once",
                "More than this wait in the notification list.",
                Control::Slider { min: TOASTS_MIN as f64, max: TOASTS_MAX as f64, integer: true, value: self.max_toasts as f64 },
            ),
        ]
    }

    /// A change made on the page, as the page sent it.
    pub fn apply(&self, id: &str, change: &Change) -> Result<NotificationPrefs, PrefsError> {
        match (id, change) {
            ("dnd" | "quiet", Change::Switch(on)) => self.set(id, &json!(on)),
            (_, Change::Number(n)) if n.fract() == 0.0 && *n >= 0.0 => {
                self.set(id, &json!(*n as u64))
            }
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
        assert!(p.set("dnd", &json!(true)).unwrap().dnd);
        assert_eq!(p.set("dnd", &json!(1)), Err(PrefsError::BadValue));
        assert_eq!(
            p.set("position", &json!(1)),
            Err(PrefsError::UnknownSetting)
        );
    }

    #[test]
    fn the_page_shows_every_setting_once_and_applies_what_it_sends() {
        let p = NotificationPrefs::default();
        let ids: Vec<String> = p.rows().into_iter().map(|r| r.id).collect();
        assert_eq!(
            ids,
            [
                "dnd",
                "quiet",
                "quiet_start",
                "quiet_end",
                "toast_seconds",
                "max_toasts"
            ]
        );
        assert!(p.apply("dnd", &Change::Switch(true)).unwrap().dnd);
        assert!(p.apply("quiet", &Change::Switch(true)).unwrap().quiet);
        assert_eq!(
            p.apply("quiet_start", &Change::Number(9.0))
                .unwrap()
                .quiet_start,
            9
        );
        assert_eq!(
            p.apply("quiet_end", &Change::Number(24.0)),
            Err(PrefsError::BadValue)
        );
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
            dnd: true,
            quiet: true,
            quiet_start: 22,
            quiet_end: 7,
        };
        assert_eq!(NotificationPrefs::parse(&p.to_json()), p);
    }

    #[test]
    fn quiet_hours_cover_the_evening_and_stop_at_the_end_hour() {
        let on = NotificationPrefs {
            quiet: true,
            quiet_start: 22,
            quiet_end: 7,
            ..NotificationPrefs::default()
        };
        assert!(on.quiet_at(22));
        assert!(on.quiet_at(0));
        assert!(on.quiet_at(6));
        assert!(!on.quiet_at(7));
        assert!(!on.quiet_at(21));
        let day = NotificationPrefs {
            quiet: true,
            quiet_start: 9,
            quiet_end: 17,
            ..NotificationPrefs::default()
        };
        assert!(day.quiet_at(9));
        assert!(day.quiet_at(16));
        assert!(!day.quiet_at(17));
        assert!(!day.quiet_at(8));
        let empty = NotificationPrefs {
            quiet: true,
            quiet_start: 9,
            quiet_end: 9,
            ..NotificationPrefs::default()
        };
        assert!(!empty.quiet_at(9));
        let off = NotificationPrefs {
            quiet: false,
            quiet_start: 0,
            quiet_end: 23,
            ..NotificationPrefs::default()
        };
        assert!(!off.quiet_at(12));
        assert_eq!(
            NotificationPrefs::parse(r#"{"toast_seconds":6,"max_toasts":3,"dnd":false}"#),
            NotificationPrefs::default()
        );
    }
}
