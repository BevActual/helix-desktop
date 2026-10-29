//! Actions any app can add (`docs/design/north-star.md`, "open by design"): a provider describes its actions in
//! `helix/actions/<provider>.json` in a data directory and carries them out over D-Bus (`org.helix.ActionProvider1.Invoke(action, params)`
//! → a JSON reply). The desktop lists them with the built-in actions and checks every call with the same grants, confirmation and audit.
//!
//! A manifest says how risky its actions are, but one in the person's own folder (anything could have written it) cannot say less than
//! `act`: only manifests installed with the system may declare read-only actions. Ids are the provider's own (`<provider>.<verb>`), and the
//! built-in providers' names (`settings`, `models`) cannot be taken.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::{Action, Risk};

const BUILT_IN: [&str; 2] = ["settings", "models"];

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Provider {
    /// Lower-case letters, digits and dashes: the first part of each action id.
    pub id: String,
    pub title: String,
    /// Where `Invoke` is called.
    pub bus: String,
    pub path: String,
    /// The application ids of the app's windows: the Magic Pointer offers the app's actions that need nothing over its windows.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub apps: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Manifest {
    pub provider: Provider,
    pub actions: Vec<Action>,
}

/// Where a manifest came from: the person's own data folder, or the system's.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Origin {
    User,
    System,
}

fn name_ok(text: &str) -> bool {
    !text.is_empty()
        && text.len() <= 40
        && text
            .chars()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-')
}

fn bus_ok(bus: &str) -> bool {
    bus.len() <= 255
        && bus.split('.').count() >= 2
        && bus.split('.').all(|part| {
            !part.is_empty()
                && part
                    .chars()
                    .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
        })
}

fn path_ok(path: &str) -> bool {
    path.starts_with('/')
        && path.len() <= 255
        && path
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '/')
}

/// The manifest as Helix will use it, or why it is refused.
pub fn parse(text: &str, origin: Origin) -> Result<Manifest, String> {
    let mut manifest: Manifest =
        serde_json::from_str(text).map_err(|e| format!("not a manifest: {e}"))?;
    let provider = manifest.provider.id.clone();
    if !name_ok(&provider) {
        return Err("the provider id must be lower-case letters, digits and dashes".into());
    }
    if BUILT_IN.contains(&provider.as_str()) {
        return Err(format!("\"{provider}\" belongs to Helix"));
    }
    if !bus_ok(&manifest.provider.bus) || !path_ok(&manifest.provider.path) {
        return Err("the provider's bus name or object path is not valid".into());
    }
    manifest
        .provider
        .apps
        .retain(|app| !app.is_empty() && app.len() <= 255);
    manifest.provider.apps.truncate(20);
    if manifest.actions.is_empty() || manifest.actions.len() > 100 {
        return Err("a provider offers between 1 and 100 actions".into());
    }
    for action in &mut manifest.actions {
        let verb = action
            .id
            .strip_prefix(&format!("{provider}."))
            .ok_or_else(|| format!("action \"{}\" must start with \"{provider}.\"", action.id))?;
        if !name_ok(verb) {
            return Err(format!(
                "action \"{}\": the verb must be lower-case letters, digits and dashes",
                action.id
            ));
        }
        if !action.params.is_object() {
            return Err(format!(
                "action \"{}\": params must be a JSON Schema object",
                action.id
            ));
        }
        if let Some(param) = &action.selection {
            let declared = action
                .params
                .pointer(&format!("/properties/{param}/type"))
                .and_then(Value::as_str)
                == Some("string");
            if !declared {
                return Err(format!(
                    "action \"{}\": \"selection\" must name one of its string parameters",
                    action.id
                ));
            }
        }
        action.title = action.title.chars().take(60).collect();
        action.summary = action.summary.chars().take(400).collect();
        // Anything could have written a file in the person's folder: it may not call its actions harmless.
        if origin == Origin::User && matches!(action.risk, Risk::Observe | Risk::ObservePrivate) {
            action.risk = Risk::Act;
        }
    }
    Ok(manifest)
}

/// The manifests in `helix/actions` under each data directory, the person's first; a provider id already seen is not loaded again,
/// and a broken manifest is skipped (and said, on stderr).
pub fn load(user_data: &Path, system_data: &[PathBuf]) -> Vec<Manifest> {
    let mut found: Vec<Manifest> = Vec::new();
    let mut dirs = vec![(user_data.to_path_buf(), Origin::User)];
    dirs.extend(system_data.iter().map(|d| (d.clone(), Origin::System)));
    for (dir, origin) in dirs {
        let Ok(entries) = std::fs::read_dir(dir.join("helix").join("actions")) else {
            continue;
        };
        let mut paths: Vec<PathBuf> = entries
            .flatten()
            .map(|e| e.path())
            .filter(|p| p.extension().is_some_and(|x| x == "json"))
            .collect();
        paths.sort();
        for path in paths {
            let Ok(text) = std::fs::read_to_string(&path) else {
                continue;
            };
            match parse(&text, origin) {
                Ok(manifest) if !found.iter().any(|m| m.provider.id == manifest.provider.id) => {
                    found.push(manifest)
                }
                Ok(_) => {}
                Err(why) => eprintln!("helix-actions: {}: {why}", path.display()),
            }
        }
    }
    found
}

/// `XDG_DATA_HOME` and `XDG_DATA_DIRS`, with their usual defaults.
pub fn data_dirs() -> (PathBuf, Vec<PathBuf>) {
    let home = std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_default();
    let user = std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .unwrap_or_else(|| home.join(".local/share"));
    let system = std::env::var("XDG_DATA_DIRS")
        .ok()
        .filter(|v| !v.is_empty())
        .unwrap_or_else(|| "/usr/local/share:/usr/share".into())
        .split(':')
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .collect();
    (user, system)
}

/// The provider and action a catalogue id names, among the loaded manifests.
pub fn find<'a>(manifests: &'a [Manifest], id: &str) -> Option<(&'a Provider, &'a Action)> {
    manifests.iter().find_map(|m| {
        m.actions
            .iter()
            .find(|a| a.id == id)
            .map(|a| (&m.provider, a))
    })
}

/// The parameters an action requires.
fn required(action: &Action) -> Vec<&str> {
    action
        .params
        .get("required")
        .and_then(Value::as_array)
        .map(|r| r.iter().filter_map(Value::as_str).collect())
        .unwrap_or_default()
}

/// What the Magic Pointer offers from the apps: actions that take the selection (when there is one), wherever it is; and, over one of an
/// app's own windows, its actions that need nothing. Never a blocked one. Each with the parameters it would run with.
pub fn for_pointer<'a>(
    manifests: &'a [Manifest],
    app_id: Option<&str>,
    selection: Option<&str>,
) -> Vec<(&'a Provider, &'a Action, Value)> {
    let mut offered = Vec::new();
    for m in manifests {
        let own_window = app_id.is_some_and(|app| m.provider.apps.iter().any(|a| a == app));
        for action in &m.actions {
            if action.risk == Risk::Blocked {
                continue;
            }
            let needs = required(action);
            match (
                &action.selection,
                selection.filter(|s| !s.trim().is_empty()),
            ) {
                (Some(param), Some(text)) if needs.iter().all(|n| n == param) => {
                    offered.push((&m.provider, action, serde_json::json!({ param: text })));
                }
                _ if own_window && needs.is_empty() => {
                    offered.push((&m.provider, action, serde_json::json!({})));
                }
                _ => {}
            }
        }
    }
    offered
}

/// What a provider's reply may carry: a sentence for the history, and the call that undoes what it did.
pub fn reply_parts(reply: &Value) -> (Option<String>, Option<Value>) {
    let done = reply
        .get("done")
        .and_then(Value::as_str)
        .map(|s| s.chars().take(200).collect());
    let undo = reply
        .get("undo")
        .filter(|u| {
            u.get("action").and_then(Value::as_str).is_some()
                && u.get("params").is_some_and(Value::is_object)
        })
        .cloned();
    (done, undo)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn text(provider: &str, action: &str, risk: &str) -> String {
        json!({
            "provider": { "id": provider, "title": "Notes", "bus": "org.example.Notes", "path": "/org/example/Notes" },
            "actions": [{ "id": action, "title": "Add a note", "summary": "Adds a note.", "params": { "type": "object" },
                          "risk": risk, "undo": "restore" }]
        })
        .to_string()
    }

    #[test]
    fn a_good_manifest_is_read() {
        let m = parse(&text("notes", "notes.add", "act"), Origin::User).unwrap();
        assert_eq!(m.provider.bus, "org.example.Notes");
        assert_eq!(m.actions[0].id, "notes.add");
    }

    #[test]
    fn one_in_the_persons_folder_cannot_call_itself_harmless() {
        let user = parse(&text("notes", "notes.list", "observe"), Origin::User).unwrap();
        assert_eq!(user.actions[0].risk, Risk::Act);
        let system = parse(&text("notes", "notes.list", "observe"), Origin::System).unwrap();
        assert_eq!(system.actions[0].risk, Risk::Observe);
    }

    #[test]
    fn ids_are_the_providers_own_and_helixs_names_are_taken() {
        assert!(parse(&text("notes", "other.add", "act"), Origin::System).is_err());
        assert!(parse(&text("settings", "settings.set", "act"), Origin::System).is_err());
        assert!(parse(&text("Notes!", "Notes!.add", "act"), Origin::System).is_err());
        assert!(parse(&text("notes", "notes.Add Thing", "act"), Origin::System).is_err());
    }

    #[test]
    fn the_pointer_offers_selection_actions_anywhere_and_bare_ones_over_the_apps_windows() {
        let text = json!({
            "provider": { "id": "notes", "title": "Notes", "bus": "org.example.Notes", "path": "/org/example/Notes", "apps": ["org.example.Notes"] },
            "actions": [
                { "id": "notes.add", "title": "Add to Notes", "summary": "Adds a note.", "risk": "act", "undo": "restore", "selection": "text",
                  "params": { "type": "object", "properties": { "text": { "type": "string" } }, "required": ["text"] } },
                { "id": "notes.new", "title": "New note", "summary": "Opens a new note.", "risk": "act", "undo": "none",
                  "params": { "type": "object" } },
                { "id": "notes.remove", "title": "Remove a note", "summary": "Removes one.", "risk": "act", "undo": "none",
                  "params": { "type": "object", "properties": { "text": { "type": "string" } }, "required": ["text"] } }
            ]
        })
        .to_string();
        let m = vec![parse(&text, Origin::System).unwrap()];
        let ids = |app, sel| {
            for_pointer(&m, app, sel)
                .iter()
                .map(|(_, a, p)| format!("{} {p}", a.id))
                .collect::<Vec<_>>()
        };
        assert_eq!(
            ids(Some("firefox"), Some("Milk")),
            ["notes.add {\"text\":\"Milk\"}"]
        );
        assert_eq!(ids(Some("org.example.Notes"), None), ["notes.new {}"]);
        assert!(ids(Some("firefox"), Some("  ")).is_empty());
        let bad = text.replace("\"selection\":\"text\"", "\"selection\":\"nope\"");
        assert!(parse(&bad, Origin::System).is_err());
    }

    #[test]
    fn a_reply_says_what_was_done_and_how_to_undo_it() {
        let (done, undo) = reply_parts(
            &json!({ "done": "Added “Milk”", "undo": { "action": "notes.delete", "params": { "id": 3 } } }),
        );
        assert_eq!(done.as_deref(), Some("Added “Milk”"));
        assert_eq!(undo.unwrap()["action"], "notes.delete");
        assert_eq!(reply_parts(&json!({ "undo": { "action": "x" } })).1, None);
    }

    #[test]
    fn manifests_load_from_the_persons_folder_first() {
        let root = std::env::temp_dir().join(format!("helix-manifests-{}", std::process::id()));
        let (user, system) = (root.join("user"), root.join("system"));
        for (dir, risk) in [(&user, "observe"), (&system, "observe")] {
            std::fs::create_dir_all(dir.join("helix/actions")).unwrap();
            std::fs::write(
                dir.join("helix/actions/notes.json"),
                text("notes", "notes.list", risk),
            )
            .unwrap();
        }
        std::fs::write(system.join("helix/actions/broken.json"), "{").unwrap();
        let found = load(&user, &[system]);
        assert_eq!(found.len(), 1);
        assert_eq!(found[0].actions[0].risk, Risk::Act);
        let _ = std::fs::remove_dir_all(&root);
    }
}
