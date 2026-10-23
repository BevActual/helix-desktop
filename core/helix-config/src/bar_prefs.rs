//! What the top bar shows: the person's choices from Settings → Top bar, kept in `~/.config/helix/bar-settings.json`. Settings writes it and the shell
//! watches it. Reading is lenient (a missing or unreadable file, or a key that is not a boolean, means the default), so a bar can never be configured
//! into a state with nothing to open the control center or the notifications: those two buttons are not in the list.

use serde_json::{json, Map, Value};

/// One thing the bar can show or hide: its key in the file, what the page calls it, and whether it is shown unless the person says otherwise.
pub struct BarItem {
    pub key: &'static str,
    pub title: &'static str,
    pub about: &'static str,
    pub group: &'static str,
    pub default: bool,
}

pub const ITEMS: &[BarItem] = &[
    BarItem {
        key: "apps",
        title: "Apps button",
        about: "Opens the launcher. The launcher also opens from the keyboard.",
        group: "Left",
        default: true,
    },
    BarItem {
        key: "mobius",
        title: "Mobius button",
        about: "Opens Mobius. The summon key still does.",
        group: "Left",
        default: true,
    },
    BarItem {
        key: "workspaces",
        title: "Workspaces",
        about: "The dots that show which workspace you are on.",
        group: "Left",
        default: true,
    },
    BarItem {
        key: "window",
        title: "Active window",
        about: "The name of the window you are working in, beside the workspaces.",
        group: "Left",
        default: false,
    },
    BarItem {
        key: "date",
        title: "Date beside the time",
        about: "The day and date before the time.",
        group: "Clock",
        default: true,
    },
    BarItem {
        key: "seconds",
        title: "Seconds",
        about: "Shows the seconds in the time. The clock then updates every second.",
        group: "Clock",
        default: false,
    },
    BarItem {
        key: "wifi",
        title: "Wi-Fi",
        about: "The network icon in the status group.",
        group: "Status icons",
        default: true,
    },
    BarItem {
        key: "bluetooth",
        title: "Bluetooth",
        about: "Shown only while Bluetooth is on and has something to say.",
        group: "Status icons",
        default: true,
    },
    BarItem {
        key: "volume",
        title: "Sound",
        about: "The speaker icon.",
        group: "Status icons",
        default: true,
    },
    BarItem {
        key: "battery",
        title: "Battery",
        about: "The battery icon, on a computer that has one.",
        group: "Status icons",
        default: true,
    },
    BarItem {
        key: "battery_percent",
        title: "Battery percentage",
        about: "The number beside the battery icon.",
        group: "Status icons",
        default: true,
    },
];

/// The choices: a key is present only when it is a boolean, and only keys in [`ITEMS`] are kept.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BarPrefs {
    set: std::collections::BTreeMap<String, bool>,
}

impl BarPrefs {
    /// Reads the file's text; anything wrong is the default.
    pub fn parse(text: &str) -> BarPrefs {
        let mut prefs = BarPrefs::default();
        if let Some(Value::Object(map)) = serde_json::from_str::<Value>(text)
            .ok()
            .and_then(|v| v.get("show").cloned())
        {
            for item in ITEMS {
                if let Some(on) = map.get(item.key).and_then(Value::as_bool) {
                    prefs.set.insert(item.key.to_string(), on);
                }
            }
        }
        prefs
    }

    /// Whether `key` is shown (an unknown key is not).
    pub fn shown(&self, key: &str) -> bool {
        match ITEMS.iter().find(|item| item.key == key) {
            Some(item) => self.set.get(key).copied().unwrap_or(item.default),
            None => false,
        }
    }

    /// The same choices with one changed. An unknown key changes nothing.
    pub fn with(&self, key: &str, on: bool) -> BarPrefs {
        let mut next = self.clone();
        if ITEMS.iter().any(|item| item.key == key) {
            next.set.insert(key.to_string(), on);
        }
        next
    }

    pub fn to_json(&self) -> String {
        let show: Map<String, Value> = self
            .set
            .iter()
            .map(|(k, v)| (k.clone(), json!(v)))
            .collect();
        json!({"version": 1, "show": show}).to_string()
    }

    /// Every item and whether it is shown, for the page in the shell: `{"apps": true, ...}`.
    pub fn effective_json(&self) -> String {
        let all: Map<String, Value> = ITEMS
            .iter()
            .map(|item| (item.key.to_string(), json!(self.shown(item.key))))
            .collect();
        Value::Object(all).to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_hold_until_a_choice_is_made_and_the_file_is_lenient() {
        let none = BarPrefs::parse("nonsense");
        assert!(none.shown("date") && none.shown("workspaces") && !none.shown("seconds"));
        assert!(!none.shown("nothing"));
        let some = none
            .with("date", false)
            .with("seconds", true)
            .with("bogus", true);
        let back = BarPrefs::parse(&some.to_json());
        assert_eq!(back, some);
        assert!(!back.shown("date") && back.shown("seconds") && !back.shown("bogus"));
        assert!(
            BarPrefs::parse(r#"{"show":{"date":"no","apps":false}}"#).shown("date"),
            "not a boolean: the default"
        );
        assert!(!BarPrefs::parse(r#"{"show":{"apps":false}}"#).shown("apps"));
    }

    #[test]
    fn the_shell_is_told_every_item() {
        let all: Value = serde_json::from_str(&BarPrefs::default().effective_json()).unwrap();
        for item in ITEMS {
            assert_eq!(all[item.key], json!(item.default), "{}", item.key);
        }
    }
}
