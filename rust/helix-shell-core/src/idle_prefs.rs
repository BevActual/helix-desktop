//! When the session locks and the screen goes off, as the person sets it in Settings. The file is `~/.config/helix/idle.toml`, which
//! `helix-idle` reads (the same keys, the same floors). Writing keeps every other key in the file (the commands) exactly as it was.

use serde_json::json;

use crate::catalog::{Change, Control, RowView};

pub const LOCK_MIN: u32 = 1;
pub const LOCK_MAX: u32 = 120;
pub const OFF_MAX: u32 = 240;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct IdlePrefs {
    pub enabled: bool,
    pub lock_minutes: u32,
    /// `None`: the screen is never turned off.
    pub screen_off_minutes: Option<u32>,
}

impl Default for IdlePrefs {
    fn default() -> Self {
        IdlePrefs { enabled: true, lock_minutes: 5, screen_off_minutes: Some(7) }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PrefsError {
    UnknownSetting,
    BadValue,
}

fn minutes(seconds: i64) -> u32 {
    ((seconds.max(0) + 30) / 60) as u32
}

impl IdlePrefs {
    /// Reads `idle.toml` leniently: a missing or odd value is the default, times are brought inside what Settings offers.
    pub fn parse(text: &str) -> IdlePrefs {
        let defaults = IdlePrefs::default();
        let Ok(table) = text.parse::<toml::Table>() else { return defaults };
        let lock_minutes = table
            .get("lock_after_seconds")
            .and_then(toml::Value::as_integer)
            .filter(|n| *n > 0)
            .map_or(defaults.lock_minutes, |s| minutes(s).clamp(LOCK_MIN, LOCK_MAX));
        let screen_off_minutes = match table.get("screen_off_after_seconds").and_then(toml::Value::as_integer) {
            Some(0) => None,
            Some(n) if n > 0 => Some(minutes(n).clamp(lock_minutes + 1, OFF_MAX.max(lock_minutes + 1))),
            _ => defaults.screen_off_minutes.map(|m| m.max(lock_minutes + 1)),
        };
        IdlePrefs {
            enabled: table.get("enabled").and_then(toml::Value::as_bool).unwrap_or(true),
            lock_minutes,
            screen_off_minutes,
        }
    }

    /// `existing` with these values in it; everything else in the file is kept.
    pub fn write_into(&self, existing: &str) -> String {
        let mut table = existing.parse::<toml::Table>().unwrap_or_default();
        table.insert("enabled".into(), toml::Value::Boolean(self.enabled));
        table.insert("lock_after_seconds".into(), toml::Value::Integer(i64::from(self.lock_minutes) * 60));
        table.insert("screen_off_after_seconds".into(), toml::Value::Integer(self.screen_off_minutes.map_or(0, |m| i64::from(m) * 60)));
        toml::to_string(&table).unwrap_or_default()
    }

    pub fn rows(&self) -> Vec<RowView> {
        let row = |id: &str, title: &str, group: &str, description: &str, control: Control| RowView {
            id: id.into(),
            title: title.into(),
            group: group.into(),
            description: description.into(),
            overridden: false,
            control,
        };
        let off = self.screen_off_minutes;
        vec![
            row("enabled", "Lock when idle", "Lock", "Locks the session after nobody has used it for a while.", Control::Switch { on: self.enabled }),
            row("lock_minutes", "Lock after (minutes)", "Lock", "", Control::Slider { min: LOCK_MIN as f64, max: LOCK_MAX as f64, integer: true, value: self.lock_minutes as f64 }),
            row("screen_off", "Turn off the screen", "Screen", "After the session has locked.", Control::Switch { on: off.is_some() }),
            row(
                "screen_off_minutes",
                "Turn off after (minutes)",
                "Screen",
                "",
                Control::Slider {
                    min: (self.lock_minutes + 1) as f64,
                    max: OFF_MAX as f64,
                    integer: true,
                    value: off.unwrap_or(self.lock_minutes + 2).clamp(self.lock_minutes + 1, OFF_MAX) as f64,
                },
            ),
        ]
    }

    pub fn apply(&self, id: &str, change: &Change) -> Result<IdlePrefs, PrefsError> {
        let mut next = self.clone();
        let whole = |n: &f64, low: u32, high: u32| -> Result<u32, PrefsError> {
            if n.fract() != 0.0 || *n < f64::from(low) || *n > f64::from(high) {
                Err(PrefsError::BadValue)
            } else {
                Ok(*n as u32)
            }
        };
        match (id, change) {
            ("enabled", Change::Switch(on)) => next.enabled = *on,
            ("lock_minutes", Change::Number(n)) => {
                next.lock_minutes = whole(n, LOCK_MIN, LOCK_MAX)?;
                // The screen never goes off before the lock: it follows along.
                if let Some(off) = next.screen_off_minutes {
                    next.screen_off_minutes = Some(off.max(next.lock_minutes + 1));
                }
            }
            ("screen_off", Change::Switch(on)) => {
                next.screen_off_minutes = on.then(|| self.screen_off_minutes.unwrap_or(self.lock_minutes + 2).max(self.lock_minutes + 1));
            }
            ("screen_off_minutes", Change::Number(n)) => {
                next.screen_off_minutes = Some(whole(n, self.lock_minutes + 1, OFF_MAX)?);
            }
            ("enabled" | "lock_minutes" | "screen_off" | "screen_off_minutes", _) => return Err(PrefsError::BadValue),
            _ => return Err(PrefsError::UnknownSetting),
        }
        Ok(next)
    }

    pub fn summary(&self) -> serde_json::Value {
        json!({"enabled": self.enabled, "lock_minutes": self.lock_minutes, "screen_off_minutes": self.screen_off_minutes})
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nothing_or_nonsense_is_the_defaults() {
        for text in ["", "[not toml", "lock_after_seconds = \"soon\"", "lock_after_seconds = -4"] {
            assert_eq!(IdlePrefs::parse(text), IdlePrefs::default(), "{text}");
        }
    }

    #[test]
    fn it_reads_what_helix_idle_reads() {
        let p = IdlePrefs::parse("enabled = false\nlock_after_seconds = 120\nscreen_off_after_seconds = 0\n");
        assert_eq!(p, IdlePrefs { enabled: false, lock_minutes: 2, screen_off_minutes: None });
        let q = IdlePrefs::parse("lock_after_seconds = 600\nscreen_off_after_seconds = 60\n");
        assert_eq!(q.screen_off_minutes, Some(11), "the screen never goes off before the lock");
    }

    #[test]
    fn writing_keeps_the_commands() {
        let existing = "lock_command = [\"swaylock\", \"-f\"]\nenabled = true\nlock_after_seconds = 300\n";
        let p = IdlePrefs::default().apply("lock_minutes", &Change::Number(10.0)).unwrap();
        let out = p.write_into(existing);
        assert!(out.contains("swaylock"), "{out}");
        assert_eq!(IdlePrefs::parse(&out), p);
        assert_eq!(IdlePrefs::parse(&IdlePrefs::default().write_into("")), IdlePrefs::default());
    }

    #[test]
    fn changes_are_exact_and_keep_the_order_of_lock_and_screen() {
        let p = IdlePrefs::default();
        assert_eq!(p.apply("lock_minutes", &Change::Number(0.0)), Err(PrefsError::BadValue));
        assert_eq!(p.apply("lock_minutes", &Change::Number(121.0)), Err(PrefsError::BadValue));
        assert_eq!(p.apply("lock_minutes", &Change::Number(2.5)), Err(PrefsError::BadValue));
        assert_eq!(p.apply("lock_minutes", &Change::Switch(true)), Err(PrefsError::BadValue));
        assert_eq!(p.apply("screen_off_minutes", &Change::Number(5.0)), Err(PrefsError::BadValue), "not after the lock");
        assert_eq!(p.apply("screen_off_minutes", &Change::Number(6.0)).unwrap().screen_off_minutes, Some(6));
        assert_eq!(p.apply("lock_minutes", &Change::Number(30.0)).unwrap().screen_off_minutes, Some(31), "it follows the lock");
        assert_eq!(p.apply("screen_off", &Change::Switch(false)).unwrap().screen_off_minutes, None);
        assert!(p.apply("screen_off", &Change::Switch(false)).unwrap().apply("screen_off", &Change::Switch(true)).unwrap().screen_off_minutes.is_some());
        assert_eq!(p.apply("command", &Change::Text("x".into())), Err(PrefsError::UnknownSetting));
    }

    #[test]
    fn the_page_lists_four_rows_in_order() {
        let ids: Vec<String> = IdlePrefs::default().rows().into_iter().map(|r| r.id).collect();
        assert_eq!(ids, ["enabled", "lock_minutes", "screen_off", "screen_off_minutes"]);
    }
}
