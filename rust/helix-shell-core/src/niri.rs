//! What the compositor (niri) says is open, as a pure model over its event stream. The stream is one JSON object per line; the first
//! events describe everything, later ones what changed. Window titles and app ids are written by the applications, so they are only
//! ever kept as text here and sanitized by whoever shows them.

use std::collections::BTreeMap;

use serde_json::Value;

/// What is kept of what applications write, and how many windows: an application cannot make the model grow without bound.
pub const MAX_WINDOWS: usize = 512;
const MAX_APP_ID: usize = 120;
const MAX_TITLE: usize = 300;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Window {
    pub id: u64,
    pub app_id: String,
    pub title: String,
    pub workspace: Option<u64>,
    pub focused: bool,
    pub urgent: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Workspace {
    pub id: u64,
    pub idx: u64,
    pub name: Option<String>,
    pub output: Option<String>,
    pub active: bool,
    pub focused: bool,
    pub urgent: bool,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Niri {
    windows: BTreeMap<u64, Window>,
    workspaces: BTreeMap<u64, Workspace>,
}

fn text(value: &Value, key: &str, max_chars: usize) -> String {
    value
        .get(key)
        .and_then(Value::as_str)
        .unwrap_or("")
        .chars()
        .take(max_chars)
        .collect()
}

fn flag(value: &Value, key: &str) -> bool {
    value.get(key).and_then(Value::as_bool).unwrap_or(false)
}

fn window(value: &Value) -> Option<Window> {
    Some(Window {
        id: value.get("id")?.as_u64()?,
        app_id: text(value, "app_id", MAX_APP_ID),
        title: text(value, "title", MAX_TITLE),
        workspace: value.get("workspace_id").and_then(Value::as_u64),
        focused: flag(value, "is_focused"),
        urgent: flag(value, "is_urgent"),
    })
}

fn workspace(value: &Value) -> Option<Workspace> {
    Some(Workspace {
        id: value.get("id")?.as_u64()?,
        idx: value.get("idx").and_then(Value::as_u64).unwrap_or(0),
        name: value
            .get("name")
            .and_then(Value::as_str)
            .map(str::to_string),
        output: value
            .get("output")
            .and_then(Value::as_str)
            .map(str::to_string),
        active: flag(value, "is_active"),
        focused: flag(value, "is_focused"),
        urgent: flag(value, "is_urgent"),
    })
}

impl Niri {
    /// Applies one event (a parsed line of the stream). Returns whether anything this model keeps changed.
    pub fn apply(&mut self, event: &Value) -> bool {
        let Some((name, body)) = event.as_object().and_then(|o| o.iter().next()) else {
            return false;
        };
        let before = (self.windows.clone(), self.workspaces.clone());
        match name.as_str() {
            "WindowsChanged" => {
                self.windows = body
                    .get("windows")
                    .and_then(Value::as_array)
                    .map(|list| {
                        list.iter()
                            .filter_map(window)
                            .take(MAX_WINDOWS)
                            .map(|w| (w.id, w))
                            .collect()
                    })
                    .unwrap_or_default();
            }
            "WindowOpenedOrChanged" => {
                if let Some(opened) = body.get("window").and_then(window) {
                    if opened.focused {
                        for other in self.windows.values_mut() {
                            other.focused = false;
                        }
                    }
                    // A new window beyond the cap is not tracked; one already known is always updated.
                    if self.windows.len() < MAX_WINDOWS || self.windows.contains_key(&opened.id) {
                        self.windows.insert(opened.id, opened);
                    }
                }
            }
            "WindowClosed" => {
                if let Some(id) = body.get("id").and_then(Value::as_u64) {
                    self.windows.remove(&id);
                }
            }
            "WindowFocusChanged" => {
                let focused = body.get("id").and_then(Value::as_u64);
                for other in self.windows.values_mut() {
                    other.focused = Some(other.id) == focused;
                }
            }
            "WindowUrgencyChanged" => {
                if let (Some(id), Some(urgent)) = (
                    body.get("id").and_then(Value::as_u64),
                    body.get("urgent").and_then(Value::as_bool),
                ) {
                    if let Some(w) = self.windows.get_mut(&id) {
                        w.urgent = urgent;
                    }
                }
            }
            "WorkspacesChanged" => {
                self.workspaces = body
                    .get("workspaces")
                    .and_then(Value::as_array)
                    .map(|list| {
                        list.iter()
                            .filter_map(workspace)
                            .map(|w| (w.id, w))
                            .collect()
                    })
                    .unwrap_or_default();
            }
            "WorkspaceActivated" => {
                if let Some(id) = body.get("id").and_then(Value::as_u64) {
                    let focused = body
                        .get("focused")
                        .and_then(Value::as_bool)
                        .unwrap_or(false);
                    let output = self.workspaces.get(&id).and_then(|w| w.output.clone());
                    for w in self.workspaces.values_mut() {
                        if w.output == output {
                            w.active = w.id == id;
                        }
                        if focused {
                            w.focused = w.id == id;
                        }
                    }
                }
            }
            _ => {}
        }
        before != (self.windows.clone(), self.workspaces.clone())
    }

    /// Windows in the order niri gave them (by id, which is the order they were opened).
    pub fn windows(&self) -> impl Iterator<Item = &Window> {
        self.windows.values()
    }

    pub fn workspaces(&self) -> Vec<&Workspace> {
        let mut list: Vec<&Workspace> = self.workspaces.values().collect();
        list.sort_by_key(|w| (w.output.clone(), w.idx));
        list
    }

    /// The window that has the keyboard, if one does (none while a layer-shell surface such as the launcher holds it).
    pub fn focused_window(&self) -> Option<&Window> {
        self.windows.values().find(|w| w.focused)
    }

    pub fn window(&self, id: u64) -> Option<&Window> {
        self.windows.get(&id)
    }
}

/// Whether a window's app id looks like a terminal emulator. A pasted line break runs a command there unless the terminal protects
/// against it, so the palette treats these windows with extra care. A false positive only makes the palette stricter.
pub fn looks_like_terminal(app_id: &str) -> bool {
    const NAMES: &[&str] = &[
        "terminal",
        "foot",
        "kitty",
        "alacritty",
        "wezterm",
        "konsole",
        "xterm",
        "ghostty",
        "warp",
        "tilix",
        "terminator",
        "urxvt",
        "rxvt",
        "ptyxis",
        "blackbox",
        "rio",
        "st-256color",
        "cool-retro-term",
        "hyper",
        "tabby",
        "contour",
    ];
    let id = app_id.to_lowercase();
    NAMES.iter().any(|name| id.contains(name))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn win(id: u64, app: &str, focused: bool) -> Value {
        json!({"id": id, "app_id": app, "title": format!("t{id}"), "workspace_id": 1, "is_focused": focused, "is_urgent": false})
    }

    fn started() -> Niri {
        let mut n = Niri::default();
        n.apply(&json!({"WindowsChanged": {"windows": [win(1, "org.a", false), win(2, "org.b", true)]}}));
        n
    }

    #[test]
    fn the_first_event_describes_every_window() {
        let n = started();
        let ids: Vec<u64> = n.windows().map(|w| w.id).collect();
        assert_eq!(ids, [1, 2]);
        assert!(n.window(2).unwrap().focused && !n.window(1).unwrap().focused);
        assert_eq!(n.window(1).unwrap().app_id, "org.a");
    }

    #[test]
    fn opening_a_focused_window_takes_focus_from_the_others() {
        let mut n = started();
        assert!(n.apply(&json!({"WindowOpenedOrChanged": {"window": win(3, "org.c", true)}})));
        assert_eq!(n.windows().filter(|w| w.focused).count(), 1);
        assert!(n.window(3).unwrap().focused);
    }

    #[test]
    fn a_changed_window_replaces_its_old_self() {
        let mut n = started();
        let mut renamed = win(1, "org.a", false);
        renamed["title"] = json!("new title");
        assert!(n.apply(&json!({"WindowOpenedOrChanged": {"window": renamed}})));
        assert_eq!(n.window(1).unwrap().title, "new title");
        assert_eq!(n.windows().count(), 2);
    }

    #[test]
    fn closing_removes_and_focus_moves() {
        let mut n = started();
        assert!(n.apply(&json!({"WindowClosed": {"id": 2}})));
        assert!(n.window(2).is_none());
        assert!(n.apply(&json!({"WindowFocusChanged": {"id": 1}})));
        assert!(n.window(1).unwrap().focused);
        assert!(n.apply(&json!({"WindowFocusChanged": {"id": null}})));
        assert!(n.windows().all(|w| !w.focused));
    }

    #[test]
    fn urgency_follows_the_event() {
        let mut n = started();
        assert!(n.apply(&json!({"WindowUrgencyChanged": {"id": 1, "urgent": true}})));
        assert!(n.window(1).unwrap().urgent);
        assert!(
            !n.apply(&json!({"WindowUrgencyChanged": {"id": 1, "urgent": true}})),
            "no change is reported as none"
        );
    }

    #[test]
    fn workspaces_are_listed_by_output_and_position_and_activation_moves() {
        let mut n = Niri::default();
        n.apply(&json!({"WorkspacesChanged": {"workspaces": [
            {"id": 7, "idx": 2, "name": null, "output": "eDP-1", "is_active": false, "is_focused": false, "is_urgent": false},
            {"id": 5, "idx": 1, "name": "web", "output": "eDP-1", "is_active": true, "is_focused": true, "is_urgent": false},
            {"id": 9, "idx": 1, "name": null, "output": "HDMI-A-1", "is_active": true, "is_focused": false, "is_urgent": false}
        ]}}));
        let order: Vec<u64> = n.workspaces().iter().map(|w| w.id).collect();
        assert_eq!(
            order,
            [9, 5, 7],
            "grouped by output, then by position on it"
        );
        assert!(n.apply(&json!({"WorkspaceActivated": {"id": 7, "focused": true}})));
        let by: BTreeMap<u64, (bool, bool)> = n
            .workspaces()
            .iter()
            .map(|w| (w.id, (w.active, w.focused)))
            .collect();
        assert_eq!(by[&7], (true, true));
        assert_eq!(by[&5], (false, false));
        assert_eq!(
            by[&9],
            (true, false),
            "another output's active workspace is untouched"
        );
    }

    #[test]
    fn events_it_does_not_know_and_nonsense_change_nothing() {
        let mut n = started();
        assert!(!n.apply(&json!({"KeyboardLayoutsChanged": {"keyboard_layouts": {}}})));
        assert!(!n.apply(&json!("just a string")));
        assert!(!n.apply(&json!({})));
        assert!(!n.apply(&json!({"WindowClosed": {"id": "x"}})));
        assert!(!n.apply(&json!({"WindowOpenedOrChanged": {"window": {"title": "no id"}}})));
        assert_eq!(n.windows().count(), 2);
    }

    #[test]
    fn a_window_with_odd_fields_is_still_kept_as_text() {
        let mut n = Niri::default();
        n.apply(&json!({"WindowOpenedOrChanged": {"window": {"id": 4, "app_id": null, "title": "a\u{202e}b"}}}));
        let w = n.window(4).unwrap();
        assert_eq!(w.app_id, "");
        assert_eq!(w.title, "a\u{202e}b", "sanitizing is the viewer's job");
        assert_eq!(w.workspace, None);
        assert!(!w.focused && !w.urgent, "missing flags mean no");
    }

    #[test]
    fn what_applications_write_and_how_many_windows_are_bounded() {
        let mut n = Niri::default();
        n.apply(&json!({"WindowOpenedOrChanged": {"window": {"id": 1, "app_id": "a".repeat(5000), "title": "t".repeat(100_000)}}}));
        let w = n.window(1).unwrap();
        assert!(w.app_id.chars().count() <= 120 && w.title.chars().count() <= 300);
        let many: Vec<Value> = (0..2000)
            .map(|i| json!({"id": i + 10, "app_id": "x", "title": "t"}))
            .collect();
        n.apply(&json!({"WindowsChanged": {"windows": many}}));
        assert_eq!(n.windows().count(), MAX_WINDOWS);
        for i in 0..MAX_WINDOWS as u64 + 50 {
            n.apply(&json!({"WindowOpenedOrChanged": {"window": {"id": 100_000 + i, "app_id": "y", "title": "t"}}}));
        }
        assert_eq!(
            n.windows().count(),
            MAX_WINDOWS,
            "new windows beyond the cap are not tracked"
        );
        let known = n.windows().next().unwrap().id;
        n.apply(&json!({"WindowOpenedOrChanged": {"window": {"id": known, "app_id": "renamed", "title": "t"}}}));
        assert_eq!(
            n.window(known).unwrap().app_id,
            "renamed",
            "one already known is still updated"
        );
    }

    #[test]
    fn the_focused_window_is_the_one_with_the_keyboard_if_any() {
        let mut n = started();
        assert_eq!(n.focused_window().map(|w| w.id), Some(2));
        n.apply(&json!({"WindowFocusChanged": {"id": null}}));
        assert!(n.focused_window().is_none());
    }

    #[test]
    fn terminals_are_recognised_by_their_app_id() {
        for yes in [
            "Alacritty",
            "foot",
            "kitty",
            "org.wezterm.wezterm",
            "dev.warp.Warp",
            "org.gnome.Terminal",
            "com.mitchellh.ghostty",
            "org.kde.konsole",
        ] {
            assert!(looks_like_terminal(yes), "{yes}");
        }
        for no in [
            "org.gnome.Nautilus",
            "brave-browser",
            "cursor",
            "",
            "com.anthropic.Claude",
        ] {
            assert!(!looks_like_terminal(no), "{no}");
        }
    }
}
