//! What `status` tells an agent: a small JSON description of the desktop it can act on. It is an *observe*-tier answer, so nothing a program wrote
//! may be in it: no window titles, no notification text, no clipboard. Windows are listed by id, application name (from the desktop entry, or the
//! sanitized program name when the window is not an installed application) and whether it really is that application. The ids are what
//! `window-close` takes.

use serde_json::{json, Value};

use helix_base::WindowFact;

const MAX_WINDOWS: usize = 40;
const MAX_WORKSPACES: usize = 20;

#[derive(Clone, Debug, Default)]
pub struct StatusInput {
    pub panel: Option<String>,
    pub dark: Option<bool>,
    pub night_light: Option<bool>,
    pub do_not_disturb: bool,
    pub volume: Option<u8>,
    pub muted: Option<bool>,
    pub brightness: Option<u8>,
    /// `(index, active, windows)` for each workspace.
    pub workspaces: Vec<(u64, bool, usize)>,
    pub windows: Vec<WindowFact>,
}

pub fn status_json(input: &StatusInput) -> String {
    let windows: Vec<Value> = input
        .windows
        .iter()
        .take(MAX_WINDOWS)
        .map(|w| json!({"id": w.id, "app": w.app, "verified": w.known, "focused": w.focused}))
        .collect();
    let workspaces: Vec<Value> = input
        .workspaces
        .iter()
        .take(MAX_WORKSPACES)
        .map(|(index, active, count)| json!({"index": index, "active": active, "windows": count}))
        .collect();
    let mut out = serde_json::Map::new();
    out.insert("panel".into(), json!(input.panel.clone().unwrap_or_default()));
    out.insert("do_not_disturb".into(), json!(input.do_not_disturb));
    for (key, value) in [
        ("dark_theme", input.dark.map(Value::from)),
        ("night_light", input.night_light.map(Value::from)),
        ("volume", input.volume.map(Value::from)),
        ("muted", input.muted.map(Value::from)),
        ("brightness", input.brightness.map(Value::from)),
    ] {
        if let Some(value) = value {
            out.insert(key.into(), value);
        }
    }
    out.insert("workspaces".into(), Value::Array(workspaces));
    out.insert("windows".into(), Value::Array(windows));
    Value::Object(out).to_string()
}

/// What a person is told about `window-close`: which window the id is, in the shell's own words. `id` `None` is the focused window.
pub fn describe_window_close(windows: &[WindowFact], id: Option<u64>) -> String {
    let found = match id {
        Some(id) => windows.iter().find(|w| w.id == id),
        None => windows.iter().find(|w| w.focused),
    };
    match (found, id) {
        (Some(w), _) if w.known => format!("This closes a window of {}.", w.app),
        (Some(w), _) => format!("This closes a window of the program “{}”, which is not an installed application.", w.app),
        (None, Some(_)) => "This names a window that is not open.".to_string(),
        (None, None) => "No window is focused.".to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn window(id: u64, app: &str, known: bool, focused: bool) -> WindowFact {
        WindowFact { id, app: app.into(), known, focused }
    }

    #[test]
    fn the_status_lists_the_desktop_and_leaves_out_what_programs_wrote() {
        let input = StatusInput {
            panel: Some("launcher".into()),
            dark: Some(true),
            night_light: Some(false),
            do_not_disturb: true,
            volume: Some(40),
            muted: Some(false),
            brightness: Some(70),
            workspaces: vec![(1, true, 2), (2, false, 0)],
            windows: vec![window(7, "Brave", true, true), window(9, "payload", false, false)],
        };
        let value: Value = serde_json::from_str(&status_json(&input)).unwrap();
        assert_eq!(value["panel"], "launcher");
        assert_eq!(value["do_not_disturb"], true);
        assert_eq!((value["volume"].as_u64(), value["brightness"].as_u64()), (Some(40), Some(70)));
        assert_eq!(value["windows"][0], json!({"id": 7, "app": "Brave", "verified": true, "focused": true}));
        assert_eq!(value["windows"][1]["verified"], false);
        assert_eq!(value["workspaces"][1], json!({"index": 2, "active": false, "windows": 0}));
        assert!(value["windows"][0].get("title").is_none(), "a window's title is the program's own text");
    }

    #[test]
    fn the_person_is_told_which_window_an_id_is() {
        let windows = vec![window(7, "Brave", true, false), window(9, "payload", false, true)];
        assert_eq!(describe_window_close(&windows, Some(7)), "This closes a window of Brave.");
        assert!(describe_window_close(&windows, Some(9)).contains("not an installed application"));
        assert!(describe_window_close(&windows, Some(9)).contains("payload"));
        assert_eq!(describe_window_close(&windows, None), "This closes a window of the program “payload”, which is not an installed application.");
        assert_eq!(describe_window_close(&windows, Some(1)), "This names a window that is not open.");
        assert_eq!(describe_window_close(&[], None), "No window is focused.");
    }

    #[test]
    fn unavailable_things_are_absent_not_guessed() {
        let value: Value = serde_json::from_str(&status_json(&StatusInput::default())).unwrap();
        for key in ["dark_theme", "night_light", "volume", "muted", "brightness"] {
            assert!(value.get(key).is_none(), "{key}");
        }
        assert_eq!(value["windows"], json!([]));
        assert_eq!(value["panel"], "");
    }

    #[test]
    fn the_lists_are_bounded() {
        let input = StatusInput {
            windows: (0..500).map(|i| window(i, "x", true, false)).collect(),
            workspaces: (0..500).map(|i| (i, false, 0)).collect(),
            ..Default::default()
        };
        let value: Value = serde_json::from_str(&status_json(&input)).unwrap();
        assert_eq!(value["windows"].as_array().unwrap().len(), MAX_WINDOWS);
        assert_eq!(value["workspaces"].as_array().unwrap().len(), MAX_WORKSPACES);
    }
}
