//! Settings rows from the desktop service's catalog (`Configuration1.GetSettings`), as a pure model. A page lists the ids it shows; this turns
//! the matching catalog entries into rows a page can draw and is the only producer of the write that follows: an id that is not on the
//! page, a value of the wrong kind, a number outside the entry's bounds, a choice that is not one of its options, or a text with control
//! characters is refused here and never sent.

use serde::Serialize;
use serde_json::{json, Value};

use crate::display::sanitize_for_display;

const MAX_TEXT: usize = 500;

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct Choice {
    pub value: String,
    pub label: String,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(tag = "kind", rename_all = "lowercase")]
pub enum Control {
    Switch { on: bool },
    Choice { options: Vec<Choice>, current: String },
    Slider { min: f64, max: f64, integer: bool, value: f64 },
    Text { value: String },
}

#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct RowView {
    pub id: String,
    pub title: String,
    pub group: String,
    pub description: String,
    /// A local override pins this value; changing it here is still allowed.
    pub overridden: bool,
    pub control: Control,
}

/// What the person wants a row to become.
#[derive(Clone, Debug, PartialEq)]
pub enum Change {
    Switch(bool),
    Choice(String),
    Number(f64),
    Text(String),
}

/// The write to make: the setting's id and its new value, ready for `Configuration1.Update`.
#[derive(Clone, Debug, PartialEq)]
pub struct Update {
    pub id: String,
    pub value: Value,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RequestError {
    /// Not an entry the page shows (or not in the catalog).
    NotShown,
    WrongKind,
    OutOfRange,
    NotAnOption,
    BadText,
}

#[derive(Clone, Debug, Default)]
pub struct Catalog {
    entries: Vec<Value>,
}

fn text(entry: &Value, key: &str, max: usize) -> String {
    sanitize_for_display(entry.get(key).and_then(Value::as_str).unwrap_or(""), max)
}

fn number(entry: &Value, key: &str) -> Option<f64> {
    entry.get(key).and_then(Value::as_f64)
}

impl Catalog {
    pub fn set(&mut self, settings: &Value) {
        self.entries = settings.as_array().cloned().unwrap_or_default();
    }

    /// The text value of a string setting, if the catalog has it and it is not empty.
    pub fn text_of(&self, id: &str) -> Option<String> {
        self.entry(id)?.get("value")?.as_str().filter(|t| !t.is_empty()).map(str::to_string)
    }

    fn entry(&self, id: &str) -> Option<&Value> {
        self.entries.iter().find(|e| e.get("id").and_then(Value::as_str) == Some(id))
    }

    fn control(entry: &Value) -> Option<Control> {
        let kind = entry.get("type").and_then(Value::as_str)?;
        let value = entry.get("value")?;
        match kind {
            "boolean" => Some(Control::Switch { on: value.as_bool()? }),
            // Any string setting that comes with a list of options (modes, positions, layers, corner actions) is a choice.
            _ if value.is_string() && entry.get("options").and_then(Value::as_array).is_some_and(|o| !o.is_empty()) => {
                let options = entry.get("options").and_then(Value::as_array)?;
                let labels = entry.get("option-labels").and_then(Value::as_array);
                let options: Vec<Choice> = options
                    .iter()
                    .enumerate()
                    .filter_map(|(i, o)| {
                        let value = o.as_str()?;
                        let label = labels.and_then(|l| l.get(i)).and_then(Value::as_str).unwrap_or(value);
                        Some(Choice { value: sanitize_for_display(value, 60), label: sanitize_for_display(label, 60) })
                    })
                    .collect();
                (!options.is_empty()).then(|| Control::Choice { options, current: value.as_str().unwrap_or("").to_string() })
            }
            "integer" | "number" => {
                let (min, max) = (number(entry, "minimum")?, number(entry, "maximum")?);
                (max > min).then(|| Control::Slider { min, max, integer: kind == "integer", value: value.as_f64().unwrap_or(min).clamp(min, max) })
            }
            "string" => Some(Control::Text { value: sanitize_for_display(value.as_str()?, MAX_TEXT) }),
            _ => None,
        }
    }

    /// The rows for `ids`, in that order. An id the catalog lacks, or of a kind this model cannot draw, is skipped.
    pub fn rows(&self, ids: &[&str]) -> Vec<RowView> {
        ids.iter()
            .filter_map(|id| {
                let entry = self.entry(id)?;
                Some(RowView {
                    id: (*id).to_string(),
                    title: text(entry, "title", 80),
                    group: text(entry, "group", 60),
                    description: text(entry, "description", 300),
                    overridden: entry.get("overridden").and_then(Value::as_bool).unwrap_or(false),
                    control: Self::control(entry)?,
                })
            })
            .collect()
    }

    /// Validates a change to one row of a page that shows `ids`.
    pub fn update(&self, ids: &[&str], id: &str, change: Change) -> Result<Update, RequestError> {
        if !ids.contains(&id) {
            return Err(RequestError::NotShown);
        }
        let entry = self.entry(id).ok_or(RequestError::NotShown)?;
        let control = Self::control(entry).ok_or(RequestError::NotShown)?;
        let value = match (control, change) {
            (Control::Switch { .. }, Change::Switch(on)) => json!(on),
            (Control::Choice { options, .. }, Change::Choice(choice)) => {
                // Compare with what the catalog really offered, not the sanitized copy shown.
                let raw = entry.get("options").and_then(Value::as_array).into_iter().flatten().filter_map(Value::as_str);
                if !raw.clone().any(|o| o == choice) || options.is_empty() {
                    return Err(RequestError::NotAnOption);
                }
                json!(choice)
            }
            (Control::Slider { min, max, integer, .. }, Change::Number(n)) => {
                if !n.is_finite() || n < min || n > max {
                    return Err(RequestError::OutOfRange);
                }
                if integer { json!(n.round() as i64) } else { json!(n) }
            }
            (Control::Text { .. }, Change::Text(t)) => {
                if t.chars().count() > MAX_TEXT || t.chars().any(char::is_control) {
                    return Err(RequestError::BadText);
                }
                json!(t)
            }
            _ => return Err(RequestError::WrongKind),
        };
        Ok(Update { id: id.to_string(), value })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn catalog() -> Catalog {
        let mut c = Catalog::default();
        c.set(&json!([
            {"id": "theme.mode", "title": "Appearance", "group": "Colors", "type": "mode", "value": "dark", "options": ["dark", "light", "auto"], "option-labels": ["Dark", "Light", "Automatic"], "overridden": false},
            {"id": "theme.pure_black_dark", "title": "Pure black", "group": "Colors", "type": "boolean", "value": false, "description": "Darker surfaces"},
            {"id": "nightlight.temperature_night", "title": "Night temperature", "group": "Night Light", "type": "integer", "value": 3500, "minimum": 1000, "maximum": 24900},
            {"id": "wallpaper.edge", "title": "Edge", "group": "W", "type": "number", "value": 0.5, "minimum": 0, "maximum": 1},
            {"id": "shell.font_family", "title": "Font", "group": "Type", "type": "string", "value": "Inter"},
            {"id": "weird.thing", "title": "?", "group": "?", "type": "key-chords", "value": []},
            {"id": "hidden.setting", "title": "Not on the page", "group": "x", "type": "boolean", "value": true}
        ]));
        c
    }

    const PAGE: &[&str] = &["theme.mode", "theme.pure_black_dark", "nightlight.temperature_night", "wallpaper.edge", "shell.font_family", "weird.thing", "missing.id"];

    #[test]
    fn rows_follow_the_pages_order_and_skip_what_cannot_be_drawn() {
        let rows = catalog().rows(PAGE);
        let ids: Vec<&str> = rows.iter().map(|r| r.id.as_str()).collect();
        assert_eq!(ids, ["theme.mode", "theme.pure_black_dark", "nightlight.temperature_night", "wallpaper.edge", "shell.font_family"]);
    }

    #[test]
    fn each_kind_becomes_its_control() {
        let rows = catalog().rows(PAGE);
        assert_eq!(rows[0].control, Control::Choice { options: vec![Choice { value: "dark".into(), label: "Dark".into() }, Choice { value: "light".into(), label: "Light".into() }, Choice { value: "auto".into(), label: "Automatic".into() }], current: "dark".into() });
        assert_eq!(rows[1].control, Control::Switch { on: false });
        assert_eq!(rows[1].description, "Darker surfaces");
        assert_eq!(rows[2].control, Control::Slider { min: 1000.0, max: 24900.0, integer: true, value: 3500.0 });
        assert!(matches!(rows[3].control, Control::Slider { integer: false, .. }));
        assert_eq!(rows[4].control, Control::Text { value: "Inter".into() });
    }

    #[test]
    fn a_slider_needs_real_bounds_and_its_value_is_kept_inside_them() {
        let mut c = Catalog::default();
        c.set(&json!([
            {"id": "a", "title": "A", "group": "g", "type": "integer", "value": 5},
            {"id": "b", "title": "B", "group": "g", "type": "integer", "value": 5, "minimum": 3, "maximum": 3},
            {"id": "c", "title": "C", "group": "g", "type": "integer", "value": 99, "minimum": 0, "maximum": 10}
        ]));
        let rows = c.rows(&["a", "b", "c"]);
        assert_eq!(rows.len(), 1, "no bounds and empty bounds cannot be drawn as a slider");
        assert!(matches!(rows[0].control, Control::Slider { value, .. } if value == 10.0));
    }

    #[test]
    fn text_from_the_catalog_is_sanitized_for_display() {
        let mut c = Catalog::default();
        c.set(&json!([{"id": "x", "title": "Pay\u{202e}pal\n", "group": "g\u{202e}", "type": "boolean", "value": true, "description": "d\u{202e}"}]));
        let r = &c.rows(&["x"])[0];
        for t in [&r.title, &r.group, &r.description] {
            assert!(!t.contains('\u{202e}') && !t.contains('\n'), "{t:?}");
        }
    }

    #[test]
    fn valid_changes_become_the_exact_update() {
        let c = catalog();
        assert_eq!(c.update(PAGE, "theme.pure_black_dark", Change::Switch(true)), Ok(Update { id: "theme.pure_black_dark".into(), value: json!(true) }));
        assert_eq!(c.update(PAGE, "theme.mode", Change::Choice("light".into())).unwrap().value, json!("light"));
        assert_eq!(c.update(PAGE, "nightlight.temperature_night", Change::Number(4000.4)).unwrap().value, json!(4000), "integers are whole");
        assert_eq!(c.update(PAGE, "wallpaper.edge", Change::Number(0.25)).unwrap().value, json!(0.25));
        assert_eq!(c.update(PAGE, "shell.font_family", Change::Text("SF Pro".into())).unwrap().value, json!("SF Pro"));
    }

    #[test]
    fn only_ids_the_page_shows_can_be_changed() {
        let c = catalog();
        assert_eq!(c.update(PAGE, "hidden.setting", Change::Switch(false)), Err(RequestError::NotShown), "in the catalog but not on this page");
        assert_eq!(c.update(PAGE, "missing.id", Change::Switch(false)), Err(RequestError::NotShown), "on the page but not in the catalog");
        assert_eq!(c.update(PAGE, "weird.thing", Change::Switch(false)), Err(RequestError::NotShown), "of a kind this model cannot draw");
        assert_eq!(c.update(PAGE, "../etc", Change::Switch(false)), Err(RequestError::NotShown));
    }

    #[test]
    fn the_value_must_be_of_the_rows_kind() {
        let c = catalog();
        assert_eq!(c.update(PAGE, "theme.pure_black_dark", Change::Text("yes".into())), Err(RequestError::WrongKind));
        assert_eq!(c.update(PAGE, "theme.mode", Change::Switch(true)), Err(RequestError::WrongKind));
        assert_eq!(c.update(PAGE, "nightlight.temperature_night", Change::Switch(true)), Err(RequestError::WrongKind));
        assert_eq!(c.update(PAGE, "shell.font_family", Change::Number(1.0)), Err(RequestError::WrongKind));
    }

    #[test]
    fn numbers_must_be_finite_and_inside_the_bounds() {
        let c = catalog();
        for n in [999.0, 24901.0, f64::NAN, f64::INFINITY, f64::NEG_INFINITY, -5.0] {
            assert_eq!(c.update(PAGE, "nightlight.temperature_night", Change::Number(n)), Err(RequestError::OutOfRange), "{n}");
        }
        assert!(c.update(PAGE, "nightlight.temperature_night", Change::Number(1000.0)).is_ok());
        assert!(c.update(PAGE, "nightlight.temperature_night", Change::Number(24900.0)).is_ok());
    }

    #[test]
    fn a_choice_must_be_one_the_catalog_offered() {
        let c = catalog();
        assert_eq!(c.update(PAGE, "theme.mode", Change::Choice("sepia".into())), Err(RequestError::NotAnOption));
        assert_eq!(c.update(PAGE, "theme.mode", Change::Choice("Dark".into())), Err(RequestError::NotAnOption), "the label is not the value");
        assert_eq!(c.update(PAGE, "theme.mode", Change::Choice(String::new())), Err(RequestError::NotAnOption));
    }

    #[test]
    fn text_is_bounded_and_has_no_control_characters() {
        let c = catalog();
        assert_eq!(c.update(PAGE, "shell.font_family", Change::Text("a\nb".into())), Err(RequestError::BadText));
        assert_eq!(c.update(PAGE, "shell.font_family", Change::Text("a\u{7}b".into())), Err(RequestError::BadText));
        assert_eq!(c.update(PAGE, "shell.font_family", Change::Text("x".repeat(501))), Err(RequestError::BadText));
        assert!(c.update(PAGE, "shell.font_family", Change::Text("x".repeat(500))).is_ok());
    }

    #[test]
    fn any_string_with_options_is_a_choice_and_one_without_is_text() {
        let mut c = Catalog::default();
        c.set(&json!([
            {"id": "dock.position", "title": "Position", "group": "Dock", "type": "position", "value": "bottom", "options": ["top", "bottom"], "option-labels": []},
            {"id": "dock.launcher_icon", "title": "Icon", "group": "Dock", "type": "string", "value": "x", "options": []}
        ]));
        let rows = c.rows(&["dock.position", "dock.launcher_icon"]);
        assert!(matches!(&rows[0].control, Control::Choice { options, .. } if options[1].label == "bottom"), "a missing label falls back to the value");
        assert!(matches!(rows[1].control, Control::Text { .. }));
        assert!(c.update(&["dock.position"], "dock.position", Change::Choice("left".into())).is_err());
        assert!(c.update(&["dock.position"], "dock.position", Change::Choice("top".into())).is_ok());
    }

    #[test]
    fn text_of_gives_only_non_empty_text() {
        let mut c = Catalog::default();
        c.set(&json!([{"id": "a", "type": "string", "value": "/x"}, {"id": "b", "type": "string", "value": ""}, {"id": "c", "type": "boolean", "value": true}]));
        assert_eq!(c.text_of("a"), Some("/x".into()));
        assert_eq!(c.text_of("b"), None);
        assert_eq!(c.text_of("c"), None);
        assert_eq!(c.text_of("missing"), None);
    }

    #[test]
    fn a_nonsense_catalog_gives_no_rows() {
        let mut c = Catalog::default();
        c.set(&json!("not a list"));
        assert!(c.rows(PAGE).is_empty());
        assert_eq!(c.update(PAGE, "theme.mode", Change::Choice("dark".into())), Err(RequestError::NotShown));
    }
}
