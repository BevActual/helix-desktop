//! The quick-settings tiles that are plain settings of the desktop service: dark theme and night light. The model reads the service's
//! settings list (`Configuration1.GetSettings`) and is the only place that builds an update for them. Nothing here talks to D-Bus.
//!
//! A tile is absent (`None`) when the service does not list its setting or lists it with an unexpected type or value, so the page shows no
//! tile rather than a tile that does nothing.

use serde::Serialize;
use serde_json::{json, Value};

const THEME_MODE: &str = "theme.mode";
const NIGHT_LIGHT: &str = "nightlight.enabled";
const MODES: [&str; 3] = ["dark", "light", "auto"];

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Quick {
    /// One of `dark`, `light`, `auto`.
    theme: Option<String>,
    night_light: Option<bool>,
    /// What the theme draws as right now (`auto` resolved by the day and night schedule). Set by the host, which knows the time.
    resolved_dark: Option<bool>,
}

/// One Configuration1.Update call: the setting id and its new value.
#[derive(Clone, Debug, PartialEq)]
pub struct SettingUpdate {
    pub id: &'static str,
    pub value: Value,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RequestError {
    /// The service does not offer this setting.
    Unavailable,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct QuickView {
    /// `None`: no such tile. Otherwise whether it is on.
    pub dark: Option<bool>,
    /// "Dark", "Light" or "Automatic".
    pub dark_label: Option<&'static str>,
    pub night_light: Option<bool>,
    /// Whether the desktop draws dark right now. Differs from `dark` under Automatic, where the tile is off but the theme follows
    /// the time of day. `None` when there is no theme setting.
    pub resolved_dark: Option<bool>,
}

fn find<'a>(entries: &'a [Value], id: &str) -> Option<&'a Value> {
    entries
        .iter()
        .find(|entry| entry.get("id").and_then(Value::as_str) == Some(id))
}

impl Quick {
    /// `settings` is the list `GetSettings` returned (an array of entries). Anything else is "no tiles".
    pub fn from_settings(settings: &Value) -> Self {
        let Some(entries) = settings.as_array() else {
            return Self::default();
        };
        let theme = find(entries, THEME_MODE)
            .filter(|entry| entry.get("type").and_then(Value::as_str) == Some("mode"))
            .and_then(|entry| entry.get("value").and_then(Value::as_str))
            .filter(|mode| MODES.contains(mode))
            .map(str::to_string);
        let night_light = find(entries, NIGHT_LIGHT)
            .filter(|entry| entry.get("type").and_then(Value::as_str) == Some("boolean"))
            .and_then(|entry| entry.get("value").and_then(Value::as_bool));
        Self {
            theme,
            night_light,
            resolved_dark: None,
        }
    }

    /// `dark`, `light`, or `auto`. `None` when the service does not offer the setting.
    pub fn mode(&self) -> Option<&str> {
        self.theme.as_deref()
    }

    /// Records what `auto` resolves to at the current time (the model does not read the clock).
    pub fn set_resolved_dark(&mut self, dark: bool) {
        self.resolved_dark = Some(dark);
    }

    pub fn view(&self) -> QuickView {
        QuickView {
            dark: self.theme.as_deref().map(|mode| mode == "dark"),
            dark_label: self.theme.as_deref().map(|mode| match mode {
                "dark" => "Dark",
                "light" => "Light",
                _ => "Automatic",
            }),
            night_light: self.night_light,
            resolved_dark: self.theme.as_deref().map(|mode| match mode {
                "dark" => true,
                "light" => false,
                _ => self.resolved_dark.unwrap_or(true),
            }),
        }
    }

    /// Dark on sets the dark theme; off sets the light one (a tile is on or off, so "automatic" is changed from the settings app).
    pub fn set_dark(&self, dark: bool) -> Result<SettingUpdate, RequestError> {
        self.theme.as_ref().ok_or(RequestError::Unavailable)?;
        Ok(SettingUpdate {
            id: THEME_MODE,
            value: json!(if dark { "dark" } else { "light" }),
        })
    }

    /// Follow the schedule (light by day, dark by night) instead of a fixed mode.
    pub fn set_auto(&self) -> Result<SettingUpdate, RequestError> {
        self.theme.as_ref().ok_or(RequestError::Unavailable)?;
        Ok(SettingUpdate {
            id: THEME_MODE,
            value: json!("auto"),
        })
    }

    pub fn set_night_light(&self, on: bool) -> Result<SettingUpdate, RequestError> {
        self.night_light.ok_or(RequestError::Unavailable)?;
        Ok(SettingUpdate {
            id: NIGHT_LIGHT,
            value: json!(on),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn settings(theme: &str, night: bool) -> Value {
        json!([
            {"id": "audio.sound_volume", "type": "number", "value": 0.5},
            {"id": "theme.mode", "type": "mode", "value": theme, "options": ["dark", "light", "auto"]},
            {"id": "nightlight.enabled", "type": "boolean", "value": night},
        ])
    }

    #[test]
    fn tiles_reflect_the_settings() {
        let view = Quick::from_settings(&settings("dark", false)).view();
        assert_eq!(
            (view.dark, view.dark_label, view.night_light),
            (Some(true), Some("Dark"), Some(false))
        );
        let view = Quick::from_settings(&settings("light", true)).view();
        assert_eq!(
            (view.dark, view.dark_label, view.night_light),
            (Some(false), Some("Light"), Some(true))
        );
        let quick = Quick::from_settings(&settings("auto", true));
        assert_eq!(quick.mode(), Some("auto"));
        let view = quick.view();
        assert_eq!(
            (view.dark, view.dark_label),
            (Some(false), Some("Automatic")),
            "automatic is not dark"
        );
    }

    #[test]
    fn updates_name_the_setting_and_the_value() {
        let quick = Quick::from_settings(&settings("dark", false));
        assert_eq!(
            quick.set_dark(false).unwrap(),
            SettingUpdate {
                id: "theme.mode",
                value: json!("light")
            }
        );
        assert_eq!(
            quick.set_dark(true).unwrap(),
            SettingUpdate {
                id: "theme.mode",
                value: json!("dark")
            }
        );
        assert_eq!(
            quick.set_night_light(true).unwrap(),
            SettingUpdate {
                id: "nightlight.enabled",
                value: json!(true)
            }
        );
        assert_eq!(quick.set_night_light(false).unwrap().value, json!(false));
    }

    #[test]
    fn automatic_draws_what_the_host_resolved_and_the_tile_stays_off() {
        let mut quick = Quick::from_settings(&settings("auto", false));
        assert_eq!(
            quick.view().resolved_dark,
            Some(true),
            "until the host says, dark"
        );
        quick.set_resolved_dark(false);
        let view = quick.view();
        assert_eq!((view.dark, view.resolved_dark), (Some(false), Some(false)));
        quick.set_resolved_dark(true);
        assert_eq!(quick.view().resolved_dark, Some(true));
        let mut fixed = Quick::from_settings(&settings("light", false));
        fixed.set_resolved_dark(true);
        assert_eq!(
            fixed.view().resolved_dark,
            Some(false),
            "a fixed mode ignores the clock"
        );
        assert_eq!(
            Quick::from_settings(&settings("dark", false))
                .view()
                .resolved_dark,
            Some(true)
        );
    }

    #[test]
    fn a_missing_or_odd_setting_is_no_tile_and_no_update() {
        for data in [
            json!([]),
            json!({}),
            json!(null),
            json!("x"),
            json!([{"id": "audio.sound_volume"}]),
        ] {
            let quick = Quick::from_settings(&data);
            assert_eq!(
                quick.view(),
                QuickView {
                    dark: None,
                    dark_label: None,
                    resolved_dark: None,
                    night_light: None
                },
                "{data}"
            );
            assert_eq!(quick.set_dark(true), Err(RequestError::Unavailable));
            assert_eq!(quick.set_night_light(true), Err(RequestError::Unavailable));
        }
        // Wrong type, or a value outside what the setting allows, is not trusted.
        let odd = json!([
            {"id": "theme.mode", "type": "string", "value": "dark"},
            {"id": "nightlight.enabled", "type": "string", "value": "yes"},
        ]);
        assert_eq!(Quick::from_settings(&odd).view().dark, None);
        assert_eq!(Quick::from_settings(&odd).view().night_light, None);
        let bad_mode = json!([{"id": "theme.mode", "type": "mode", "value": "purple"}]);
        assert_eq!(Quick::from_settings(&bad_mode).view().dark, None);
        let bad_night = json!([{"id": "nightlight.enabled", "type": "boolean", "value": 1}]);
        assert_eq!(Quick::from_settings(&bad_night).view().night_light, None);
    }

    #[test]
    fn only_one_of_the_two_may_be_missing() {
        let only_theme = json!([{"id": "theme.mode", "type": "mode", "value": "light"}]);
        let quick = Quick::from_settings(&only_theme);
        assert!(quick.set_dark(true).is_ok());
        assert_eq!(quick.set_night_light(true), Err(RequestError::Unavailable));
    }
}
