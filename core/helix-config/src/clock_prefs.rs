//! How the shell writes the time of day: by the language's own habit, or always 12-hour, or always 24-hour. One value, kept in
//! `~/.config/helix/clock-settings.json`; Settings writes it and the shell watches it. Reading is lenient (anything unknown is "auto").

use serde_json::{json, Value};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ClockFormat {
    /// The language's usual way (the web view's own locale).
    #[default]
    Auto,
    Hours12,
    Hours24,
}

impl ClockFormat {
    pub fn name(self) -> &'static str {
        match self {
            ClockFormat::Auto => "auto",
            ClockFormat::Hours12 => "12",
            ClockFormat::Hours24 => "24",
        }
    }

    pub fn from_name(name: &str) -> Option<ClockFormat> {
        match name {
            "auto" => Some(ClockFormat::Auto),
            "12" => Some(ClockFormat::Hours12),
            "24" => Some(ClockFormat::Hours24),
            _ => None,
        }
    }

    /// Reads the file's text; a bad or missing value is `Auto`.
    pub fn parse(text: &str) -> ClockFormat {
        serde_json::from_str::<Value>(text)
            .ok()
            .and_then(|v| {
                v.get("format")
                    .and_then(Value::as_str)
                    .and_then(Self::from_name)
            })
            .unwrap_or_default()
    }

    pub fn to_json(self) -> String {
        json!({"version": 1, "format": self.name()}).to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips_and_is_lenient() {
        for format in [
            ClockFormat::Auto,
            ClockFormat::Hours12,
            ClockFormat::Hours24,
        ] {
            assert_eq!(ClockFormat::parse(&format.to_json()), format);
        }
        assert_eq!(ClockFormat::parse("nonsense"), ClockFormat::Auto);
        assert_eq!(ClockFormat::parse(r#"{"format":"13"}"#), ClockFormat::Auto);
        assert_eq!(ClockFormat::from_name("24"), Some(ClockFormat::Hours24));
        assert_eq!(ClockFormat::from_name("x"), None);
    }
}
