//! Startup apps: the programs that start when the person logs in, which are the desktop entries in `~/.config/autostart` and the system's
//! (`/etc/xdg/autostart`). Helix's session is a systemd one, so `systemd-xdg-autostart-generator` is what starts them and what honours `Hidden=true`.
//! A system entry is turned off the standard way: a file of the same name in the person's folder with `Hidden=true`; turning it back on removes that
//! file. A program the person adds is a copy of its desktop file. The person's files are the only ones ever written.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use helix_config::catalog::{Choice, Control, RowView};
use serde_json::Value;

use crate::defaults::{desktop_id_ok, entry_name};

/// One entry that would start at login.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    pub id: String,
    pub name: String,
    pub about: String,
    pub enabled: bool,
    /// Whether the person has a file of their own for it (as opposed to the system's alone).
    pub mine: bool,
    /// Whether the system also has one, so that removing the person's file brings the system's back rather than removing the program.
    pub system: bool,
}

fn config_home() -> Option<PathBuf> {
    std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))
}

pub fn user_dir() -> Option<PathBuf> {
    config_home().map(|c| c.join("autostart"))
}

fn system_dirs() -> Vec<PathBuf> {
    if let Some(only) = std::env::var_os("HELIX_SETTINGS_AUTOSTART_SYSTEM_DIRS") {
        return std::env::split_paths(&only).collect();
    }
    std::env::var("XDG_CONFIG_DIRS")
        .ok()
        .filter(|v| !v.is_empty())
        .unwrap_or_else(|| "/etc/xdg".into())
        .split(':')
        .map(|d| PathBuf::from(d).join("autostart"))
        .collect()
}

/// The applications that can be added: `(desktop id, name)`, by name.
pub fn installed(dirs: &[PathBuf]) -> Vec<(String, String)> {
    let mut found: BTreeMap<String, String> = BTreeMap::new();
    for dir in dirs {
        let Ok(read) = std::fs::read_dir(dir.join("applications")) else {
            continue;
        };
        for file in read.flatten() {
            let id = file.file_name().to_string_lossy().into_owned();
            if !desktop_id_ok(&id) || found.contains_key(&id) {
                continue;
            }
            let Ok(text) = std::fs::read_to_string(file.path()) else {
                continue;
            };
            if !is_application(&text) {
                continue;
            }
            if let Some(name) = entry_name(&text) {
                found.insert(id, name);
            }
        }
    }
    let mut list: Vec<(String, String)> = found.into_iter().collect();
    list.sort_by_key(|(_, name)| name.to_lowercase());
    list.truncate(500);
    list
}

fn data_dirs() -> Vec<PathBuf> {
    if let Some(only) = std::env::var_os("HELIX_SETTINGS_APPS_DIRS").filter(|v| !v.is_empty()) {
        return std::env::split_paths(&only).collect();
    }
    let home = std::env::var_os("XDG_DATA_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share")));
    let system = std::env::var("XDG_DATA_DIRS")
        .ok()
        .filter(|v| !v.is_empty())
        .unwrap_or_else(|| "/usr/local/share:/usr/share".into());
    home.into_iter()
        .chain(system.split(':').map(PathBuf::from))
        .collect()
}

fn is_application(text: &str) -> bool {
    let mut inside = false;
    let (mut application, mut exec) = (false, false);
    for line in text.lines() {
        let line = line.trim();
        if let Some(section) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            inside = section == "Desktop Entry";
        } else if inside {
            application |= line == "Type=Application";
            exec |= line.starts_with("Exec=") && line.len() > 5;
        }
    }
    application && exec
}

/// `key` of the `[Desktop Entry]` section.
fn field(text: &str, key: &str) -> Option<String> {
    let mut inside = false;
    for line in text.lines() {
        let line = line.trim();
        if let Some(section) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            inside = section == "Desktop Entry";
        } else if inside {
            if let Some(value) = line
                .strip_prefix(key)
                .and_then(|rest| rest.strip_prefix('='))
            {
                return Some(
                    value
                        .trim()
                        .chars()
                        .filter(|c| !c.is_control())
                        .take(160)
                        .collect(),
                );
            }
        }
    }
    None
}

fn disabled(text: &str) -> bool {
    field(text, "Hidden").is_some_and(|v| v.eq_ignore_ascii_case("true"))
        || field(text, "X-GNOME-Autostart-enabled").is_some_and(|v| v.eq_ignore_ascii_case("false"))
}

/// Everything that would start (or would, were it not turned off), the person's own entries first within each kind, by name.
pub fn read(user: &Path, system: &[PathBuf]) -> Vec<Entry> {
    let mut by_id: BTreeMap<String, Entry> = BTreeMap::new();
    let mut visit = |dir: &Path, mine: bool| {
        let Ok(read) = std::fs::read_dir(dir) else {
            return;
        };
        for file in read.flatten() {
            let id = file.file_name().to_string_lossy().into_owned();
            if !desktop_id_ok(&id) {
                continue;
            }
            let Ok(text) = std::fs::read_to_string(file.path()) else {
                continue;
            };
            let off = disabled(&text);
            let entry = by_id.entry(id.clone()).or_insert_with(|| Entry {
                id: id.clone(),
                name: String::new(),
                about: String::new(),
                enabled: true,
                mine: false,
                system: false,
            });
            if mine {
                // The person's file decides whether it starts, and its name wins when it has one.
                entry.mine = true;
                entry.enabled = !off;
                if let Some(name) = field(&text, "Name") {
                    entry.name = name;
                }
            } else {
                entry.system = true;
                if entry.name.is_empty() {
                    entry.name = field(&text, "Name").unwrap_or_default();
                }
                if !entry.mine {
                    entry.enabled = !off;
                }
            }
            if entry.about.is_empty() {
                entry.about = field(&text, "Comment").unwrap_or_default();
            }
        }
    };
    // System first so the person's file is applied over it.
    for dir in system.iter().rev() {
        visit(dir, false);
    }
    visit(user, true);
    let mut list: Vec<Entry> = by_id
        .into_values()
        .map(|mut e| {
            if e.name.is_empty() {
                e.name = e.id.trim_end_matches(".desktop").to_string();
            }
            e
        })
        .collect();
    list.sort_by_key(|e| e.name.to_lowercase());
    list
}

/// The page: a switch for each, and a list to add from.
pub fn rows(entries: &[Entry], addable: &[(String, String)]) -> Vec<RowView> {
    let mut rows: Vec<RowView> = entries
        .iter()
        .map(|e| RowView {
            id: format!("start:{}", e.id),
            title: e.name.clone(),
            group: "Starts when you log in".into(),
            description: match (e.about.is_empty(), e.mine && !e.system) {
                (false, _) => e.about.clone(),
                (true, true) => "Added by you.".into(),
                (true, false) => "From the system.".into(),
            },
            overridden: false,
            control: Control::Switch { on: e.enabled },
        })
        .collect();
    let have: Vec<&str> = entries.iter().map(|e| e.id.as_str()).collect();
    let options: Vec<Choice> = addable
        .iter()
        .filter(|(id, _)| !have.contains(&id.as_str()))
        .map(|(id, name)| Choice {
            value: id.clone(),
            label: name.clone(),
        })
        .collect();
    if !options.is_empty() {
        rows.push(RowView {
            id: "add:app".into(),
            title: "Add a program".into(),
            group: "Add".into(),
            description:
                "Pick one of the applications on this computer. It starts at your next login."
                    .into(),
            overridden: false,
            control: Control::Choice {
                options,
                current: String::new(),
            },
        });
    }
    rows
}

fn write_atomic(path: &Path, text: &str) -> Result<(), String> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let temporary = path.with_extension(format!("{}.tmp", std::process::id()));
    std::fs::write(&temporary, text).map_err(|e| e.to_string())?;
    std::fs::rename(&temporary, path).map_err(|e| e.to_string())
}

/// `text` with `Hidden=true` set or removed in its `[Desktop Entry]` section; everything else is kept as it was.
pub fn with_hidden(text: &str, hidden: bool) -> String {
    let mut out = Vec::new();
    let mut inside = false;
    let mut placed = false;
    for line in text.lines() {
        let trimmed = line.trim();
        if let Some(section) = trimmed.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            if inside && hidden && !placed {
                out.push("Hidden=true".to_string());
                placed = true;
            }
            inside = section == "Desktop Entry";
        } else if inside
            && (trimmed.starts_with("Hidden=") || trimmed.starts_with("X-GNOME-Autostart-enabled="))
        {
            continue;
        }
        out.push(line.to_string());
    }
    if inside && hidden && !placed {
        out.push("Hidden=true".to_string());
    }
    let mut joined = out.join("\n");
    joined.push('\n');
    joined
}

/// Turns one entry on or off.
pub fn set_enabled(user: &Path, system: &[PathBuf], id: &str, on: bool) -> Result<(), String> {
    if !desktop_id_ok(id) {
        return Err("unknown program".into());
    }
    let entries = read(user, system);
    let entry = entries
        .iter()
        .find(|e| e.id == id)
        .ok_or("unknown program")?;
    let mine = user.join(id);
    if on {
        if entry.mine && entry.system {
            // The person's file was only the switch: take it away and the system's entry applies again.
            let text = std::fs::read_to_string(&mine).unwrap_or_default();
            if field(&text, "Exec").is_none() || with_hidden(&text, false).lines().count() <= 4 {
                return std::fs::remove_file(&mine).map_err(|e| e.to_string());
            }
        }
        if entry.mine {
            let text = std::fs::read_to_string(&mine).map_err(|e| e.to_string())?;
            return write_atomic(&mine, &with_hidden(&text, false));
        }
        return Ok(());
    }
    if entry.mine {
        let text = std::fs::read_to_string(&mine).map_err(|e| e.to_string())?;
        return write_atomic(&mine, &with_hidden(&text, true));
    }
    let text = format!(
        "[Desktop Entry]\nType=Application\nName={}\nHidden=true\n",
        entry.name
    );
    write_atomic(&mine, &text)
}

/// Adds an installed application: its desktop file, copied.
pub fn add(user: &Path, apps: &[PathBuf], id: &str) -> Result<(), String> {
    if !desktop_id_ok(id) {
        return Err("unknown program".into());
    }
    if user.join(id).exists() {
        return Err("it is already on the list".into());
    }
    for dir in apps {
        if let Ok(text) = std::fs::read_to_string(dir.join("applications").join(id)) {
            if !is_application(&text) {
                return Err("that is not a program".into());
            }
            return write_atomic(&user.join(id), &with_hidden(&text, false));
        }
    }
    Err("that program is not installed".into())
}

pub fn system_folders() -> Vec<PathBuf> {
    system_dirs()
}

pub fn app_folders() -> Vec<PathBuf> {
    data_dirs()
}

/// What a change on the page asks for.
#[derive(Debug, PartialEq, Eq)]
pub enum Edit {
    Enable(String, bool),
    Add(String),
}

pub fn parse_change(id: &str, change: &Value) -> Result<Edit, String> {
    let kind = change.get("kind").and_then(Value::as_str);
    if let Some(entry) = id.strip_prefix("start:") {
        if kind != Some("switch") {
            return Err("unknown change".into());
        }
        let on = change
            .get("value")
            .and_then(Value::as_bool)
            .ok_or("not a boolean")?;
        return Ok(Edit::Enable(entry.to_string(), on));
    }
    if id == "add:app" && kind == Some("choice") {
        let app = change
            .get("value")
            .and_then(Value::as_str)
            .ok_or("not text")?;
        return Ok(Edit::Add(app.to_string()));
    }
    Err("unknown change".into())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn folder(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("helix-startup-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn put(dir: &Path, name: &str, text: &str) {
        std::fs::create_dir_all(dir).unwrap();
        std::fs::write(dir.join(name), text).unwrap();
    }

    const SYSTEM: &str =
        "[Desktop Entry]\nType=Application\nName=Keyring\nComment=Keeps passwords\nExec=keyring\n";

    #[test]
    fn a_system_entry_is_turned_off_with_a_file_of_the_persons_own_and_back_on_by_removing_it() {
        let root = folder("toggle");
        let (user, system) = (root.join("user"), vec![root.join("system")]);
        put(&system[0], "keyring.desktop", SYSTEM);
        let list = read(&user, &system);
        assert_eq!(list.len(), 1);
        assert!(list[0].enabled && !list[0].mine && list[0].system);

        set_enabled(&user, &system, "keyring.desktop", false).unwrap();
        let list = read(&user, &system);
        assert!(!list[0].enabled && list[0].mine, "{list:?}");
        assert_eq!(
            list[0].about, "Keeps passwords",
            "the system's words still show"
        );
        assert_eq!(
            std::fs::read_to_string(system[0].join("keyring.desktop")).unwrap(),
            SYSTEM,
            "the system's file is never touched"
        );

        set_enabled(&user, &system, "keyring.desktop", true).unwrap();
        assert!(
            !user.join("keyring.desktop").exists(),
            "the override is gone"
        );
        assert!(read(&user, &system)[0].enabled);
    }

    #[test]
    fn an_added_program_is_a_copy_and_can_be_turned_off_and_on_without_losing_it() {
        let root = folder("add");
        let (user, system) = (root.join("user"), vec![root.join("system")]);
        let apps = vec![root.join("data")];
        put(
            &apps[0].join("applications"),
            "tool.desktop",
            "[Desktop Entry]\nType=Application\nName=Tool\nExec=tool --x\nIcon=tool\n",
        );
        put(
            &apps[0].join("applications"),
            "hidden.desktop",
            "[Desktop Entry]\nType=Application\nName=Hid\nExec=h\nNoDisplay=true\n",
        );
        put(
            &apps[0].join("applications"),
            "link.desktop",
            "[Desktop Entry]\nType=Link\nName=L\nURL=x\n",
        );
        assert_eq!(
            installed(&apps),
            [("tool.desktop".to_string(), "Tool".to_string())]
        );

        add(&user, &apps, "tool.desktop").unwrap();
        assert!(add(&user, &apps, "tool.desktop").is_err(), "not twice");
        assert!(add(&user, &apps, "nothing.desktop").is_err());
        assert!(add(&user, &apps, "../x.desktop").is_err());
        let list = read(&user, &system);
        assert!(list[0].enabled && list[0].mine && !list[0].system);

        set_enabled(&user, &system, "tool.desktop", false).unwrap();
        assert!(!read(&user, &system)[0].enabled);
        assert!(
            std::fs::read_to_string(user.join("tool.desktop"))
                .unwrap()
                .contains("Exec=tool --x"),
            "still the program"
        );
        set_enabled(&user, &system, "tool.desktop", true).unwrap();
        let back = std::fs::read_to_string(user.join("tool.desktop")).unwrap();
        assert!(
            read(&user, &system)[0].enabled && !back.contains("Hidden"),
            "{back}"
        );
    }

    #[test]
    fn hidden_is_set_and_removed_without_touching_the_rest() {
        let text = "[Desktop Entry]\nName=A\nHidden=false\nExec=a\n\n[Desktop Action x]\nName=X\n";
        let off = with_hidden(text, true);
        assert!(
            off.contains("Hidden=true")
                && !off.contains("Hidden=false")
                && off.contains("[Desktop Action x]\nName=X")
        );
        assert_eq!(
            with_hidden(&off, false),
            "[Desktop Entry]\nName=A\nExec=a\n\n[Desktop Action x]\nName=X\n"
        );
        assert!(disabled(
            "[Desktop Entry]\nX-GNOME-Autostart-enabled=false\n"
        ));
    }

    #[test]
    fn the_rows_offer_only_what_is_not_already_there_and_changes_are_checked() {
        let entries = vec![Entry {
            id: "a.desktop".into(),
            name: "A".into(),
            about: String::new(),
            enabled: true,
            mine: true,
            system: false,
        }];
        let addable = vec![
            ("a.desktop".to_string(), "A".to_string()),
            ("b.desktop".to_string(), "B".to_string()),
        ];
        let rows = rows(&entries, &addable);
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0].description, "Added by you.");
        match &rows[1].control {
            Control::Choice { options, .. } => assert_eq!(options.len(), 1),
            _ => panic!("a choice"),
        }
        assert_eq!(
            parse_change("start:a.desktop", &json!({"kind":"switch","value":false})),
            Ok(Edit::Enable("a.desktop".into(), false))
        );
        assert_eq!(
            parse_change("add:app", &json!({"kind":"choice","value":"b.desktop"})),
            Ok(Edit::Add("b.desktop".into()))
        );
        assert!(parse_change("start:a.desktop", &json!({"kind":"choice","value":"x"})).is_err());
        assert!(parse_change("nothing", &json!({})).is_err());
    }
}
