//! Helix Shell Navigation keybinds from the Configuration catalog (`type = "key-chords"`).
//! The page shows them and proposes a replacement list; this module is the only place that
//! decides whether that list is legal before `UpdateOrderedList` is sent. Super/Meta belongs
//! to the compositor and is refused here. The service validates again.

use serde::Serialize;
use serde_json::Value;

use crate::display::sanitize_for_display;

const MAX_CHORDS: usize = 16;
const MAX_SPEC: usize = 128;

/// One navigation action and the chords currently bound to it.
#[derive(Clone, Debug, PartialEq, Serialize)]
#[cfg_attr(feature = "ts", derive(ts_rs::TS), ts(export))]
pub struct KeybindRow {
    pub id: String,
    pub title: String,
    pub chords: Vec<String>,
    pub overridden: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum KeybindError {
    NotShown,
    Empty,
    TooMany,
    BadChord,
    Duplicate,
    SuperNotAllowed,
}

/// Ids the Keyboard page may edit, in display order.
pub const KEYBIND_IDS: &[&str] = &[
    "keybinds.validate",
    "keybinds.cancel",
    "keybinds.left",
    "keybinds.right",
    "keybinds.up",
    "keybinds.down",
    "keybinds.tab_next",
    "keybinds.tab_previous",
    "keybinds.copy",
    "keybinds.save",
    "keybinds.delete",
];

fn entry<'a>(catalog: &'a [Value], id: &str) -> Option<&'a Value> {
    catalog
        .iter()
        .find(|e| e.get("id").and_then(Value::as_str) == Some(id))
}

/// Rows for the Keyboard page from a GetSettings catalog array.
pub fn view(catalog: &[Value]) -> Vec<KeybindRow> {
    KEYBIND_IDS
        .iter()
        .filter_map(|id| {
            let entry = entry(catalog, id)?;
            if entry.get("type").and_then(Value::as_str) != Some("key-chords") {
                return None;
            }
            let chords = entry
                .get("value")
                .and_then(Value::as_array)?
                .iter()
                .filter_map(|v| v.as_str().map(|s| sanitize_for_display(s, MAX_SPEC)))
                .collect::<Vec<_>>();
            Some(KeybindRow {
                id: (*id).to_string(),
                title: sanitize_for_display(
                    entry.get("title").and_then(Value::as_str).unwrap_or(id),
                    80,
                ),
                chords,
                overridden: entry
                    .get("overridden")
                    .and_then(Value::as_bool)
                    .unwrap_or(false),
            })
        })
        .collect()
}

/// Canonical form for comparing two specs (modifier order and Control/Ctrl spelling do not matter).
fn is_modifier(name: &str) -> bool {
    matches!(
        name.to_ascii_lowercase().as_str(),
        "ctrl"
            | "control"
            | "ctl"
            | "alt"
            | "option"
            | "shift"
            | "super"
            | "meta"
            | "logo"
            | "win"
            | "mod4"
            | "mod"
    )
}

fn identity(spec: &str) -> Option<String> {
    let trimmed = spec.trim();
    if trimmed.is_empty()
        || trimmed.len() > MAX_SPEC
        || trimmed.chars().any(char::is_control)
        || trimmed.starts_with('+')
        || trimmed.ends_with('+')
    {
        return None;
    }
    let parts: Vec<&str> = trimmed
        .split('+')
        .map(str::trim)
        .filter(|p| !p.is_empty())
        .collect();
    if parts.is_empty() || parts.len() != trimmed.matches('+').count() + 1 {
        return None; // empty token between pluses
    }
    let mut mods = Vec::new();
    for part in &parts[..parts.len() - 1] {
        let lower = part.to_ascii_lowercase();
        match lower.as_str() {
            "ctrl" | "control" | "ctl" => mods.push("ctrl"),
            "alt" | "option" => mods.push("alt"),
            "shift" => mods.push("shift"),
            "super" | "meta" | "logo" | "win" | "mod4" | "mod" => return None, // SuperNotAllowed at call site
            _ => return None,
        }
    }
    mods.sort_unstable();
    mods.dedup();
    let key_raw = parts[parts.len() - 1];
    if is_modifier(key_raw) {
        return None;
    }
    let key = canonical_key(key_raw);
    if key.is_empty() {
        return None;
    }
    let mut out = mods.join("+");
    if !out.is_empty() {
        out.push('+');
    }
    out.push_str(&key);
    Some(out)
}

fn canonical_key(raw: &str) -> String {
    let lower = raw.to_ascii_lowercase();
    match lower.as_str() {
        "esc" => "escape".into(),
        "del" => "delete".into(),
        "enter" | "return" => "return".into(),
        "space" | "spacebar" => "space".into(),
        "left" | "right" | "up" | "down" | "tab" => lower,
        other if other.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') => other.to_string(),
        _ => String::new(),
    }
}

fn has_super(spec: &str) -> bool {
    spec.split('+').any(|p| {
        matches!(
            p.trim().to_ascii_lowercase().as_str(),
            "super" | "meta" | "logo" | "win" | "mod4" | "mod"
        )
    })
}

/// Validates a replacement chord list for one id the page shows. Returns the candidate strings to send.
pub fn update(
    catalog: &[Value],
    id: &str,
    candidate: &[String],
) -> Result<Vec<String>, KeybindError> {
    if !KEYBIND_IDS.contains(&id) {
        return Err(KeybindError::NotShown);
    }
    let entry = entry(catalog, id).ok_or(KeybindError::NotShown)?;
    if entry.get("type").and_then(Value::as_str) != Some("key-chords") {
        return Err(KeybindError::NotShown);
    }
    if candidate.is_empty() {
        return Err(KeybindError::Empty);
    }
    if candidate.len() > MAX_CHORDS {
        return Err(KeybindError::TooMany);
    }
    let mut seen = Vec::new();
    let mut out = Vec::new();
    for raw in candidate {
        let trimmed = raw.trim();
        if trimmed.is_empty() || trimmed.len() > MAX_SPEC {
            return Err(KeybindError::BadChord);
        }
        if has_super(trimmed) {
            return Err(KeybindError::SuperNotAllowed);
        }
        let Some(ident) = identity(trimmed) else {
            return Err(KeybindError::BadChord);
        };
        if seen.contains(&ident) {
            return Err(KeybindError::Duplicate);
        }
        seen.push(ident);
        out.push(trimmed.to_string());
    }
    Ok(out)
}

/// Baseline array currently held for `id` (for UpdateOrderedList's compare-and-swap).
pub fn baseline(catalog: &[Value], id: &str) -> Option<Vec<String>> {
    let entry = entry(catalog, id)?;
    Some(
        entry
            .get("value")
            .and_then(Value::as_array)?
            .iter()
            .filter_map(|v| v.as_str().map(str::to_string))
            .collect(),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn catalog() -> Vec<Value> {
        json!([
            {"id": "keybinds.copy", "title": "Copy selected item", "type": "key-chords", "value": ["Ctrl+c"], "overridden": false},
            {"id": "keybinds.validate", "title": "Confirm or activate", "type": "key-chords", "value": ["Return"], "overridden": true},
            {"id": "theme.mode", "title": "Appearance", "type": "mode", "value": "dark"},
        ])
        .as_array()
        .unwrap()
        .clone()
    }

    #[test]
    fn view_lists_only_known_key_chords_in_order() {
        let rows = view(&catalog());
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0].id, "keybinds.validate");
        assert_eq!(rows[0].chords, ["Return"]);
        assert!(rows[0].overridden);
        assert_eq!(rows[1].id, "keybinds.copy");
        assert_eq!(rows[1].chords, ["Ctrl+c"]);
    }

    #[test]
    fn a_valid_replacement_is_accepted() {
        let next = update(
            &catalog(),
            "keybinds.copy",
            &["Alt+F9".into(), "Ctrl+F8".into()],
        )
        .unwrap();
        assert_eq!(next, ["Alt+F9", "Ctrl+F8"]);
    }

    #[test]
    fn empty_too_many_super_and_duplicates_are_refused() {
        assert_eq!(
            update(&catalog(), "keybinds.copy", &[]),
            Err(KeybindError::Empty)
        );
        let many: Vec<String> = (0..17).map(|i| format!("Ctrl+F{i}")).collect();
        assert_eq!(
            update(&catalog(), "keybinds.copy", &many),
            Err(KeybindError::TooMany)
        );
        assert_eq!(
            update(&catalog(), "keybinds.copy", &["Super+c".into()]),
            Err(KeybindError::SuperNotAllowed)
        );
        assert_eq!(
            update(
                &catalog(),
                "keybinds.copy",
                &["Ctrl+c".into(), "Control+c".into()]
            ),
            Err(KeybindError::Duplicate)
        );
        assert_eq!(
            update(&catalog(), "theme.mode", &["Ctrl+c".into()]),
            Err(KeybindError::NotShown)
        );
        assert_eq!(
            update(&catalog(), "keybinds.missing", &["Ctrl+c".into()]),
            Err(KeybindError::NotShown)
        );
    }

    #[test]
    fn bad_chords_are_refused() {
        for bad in [
            "",
            "+++",
            "Ctrl+",
            "+c",
            "Foo+c",
            "Ctrl+c\u{7}",
            "not a key",
        ] {
            assert!(
                update(&catalog(), "keybinds.copy", &[bad.into()]).is_err(),
                "{bad:?}"
            );
        }
    }
}
