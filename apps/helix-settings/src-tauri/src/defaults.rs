//! Default applications: which program opens web links, mail, folders, text, pictures, PDFs, video and music. The answer lives where every desktop
//! keeps it, `~/.config/mimeapps.list` (the freedesktop MIME-apps specification), so a choice made here is honoured by every program that asks, and one
//! made elsewhere shows here. Candidates come from the system's `mimeinfo.cache` and their `.desktop` files. Nothing is run; only the list is written.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use helix_config::catalog::{Choice, Control, RowView};
use serde_json::Value;

/// One line of the page: what is opened, and the types it stands for. The first type decides which programs are offered and what is shown.
pub struct Kind {
    pub id: &'static str,
    pub title: &'static str,
    pub mimes: &'static [&'static str],
}

pub const KINDS: &[Kind] = &[
    Kind {
        id: "web",
        title: "Web browser",
        mimes: &[
            "x-scheme-handler/https",
            "x-scheme-handler/http",
            "text/html",
        ],
    },
    Kind {
        id: "mail",
        title: "Email",
        mimes: &["x-scheme-handler/mailto"],
    },
    Kind {
        id: "files",
        title: "Files and folders",
        mimes: &["inode/directory"],
    },
    Kind {
        id: "text",
        title: "Text editor",
        mimes: &["text/plain"],
    },
    Kind {
        id: "images",
        title: "Pictures",
        mimes: &["image/png", "image/jpeg", "image/gif", "image/webp"],
    },
    Kind {
        id: "pdf",
        title: "PDF documents",
        mimes: &["application/pdf"],
    },
    Kind {
        id: "video",
        title: "Video",
        mimes: &["video/mp4", "video/x-matroska", "video/webm"],
    },
    Kind {
        id: "music",
        title: "Music",
        mimes: &["audio/mpeg", "audio/flac", "audio/ogg"],
    },
];

/// `mime=a.desktop;b.desktop;` lines of one section of a mimeapps-style file, in file order.
pub fn parse_section(text: &str, section: &str) -> BTreeMap<String, Vec<String>> {
    let mut out = BTreeMap::new();
    let mut inside = false;
    for line in text.lines() {
        let line = line.trim();
        if let Some(name) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            inside = name == section;
            continue;
        }
        if !inside || line.is_empty() || line.starts_with('#') {
            continue;
        }
        if let Some((mime, apps)) = line.split_once('=') {
            let apps: Vec<String> = apps
                .split(';')
                .map(str::trim)
                .filter(|a| !a.is_empty())
                .map(String::from)
                .collect();
            out.insert(mime.trim().to_string(), apps);
        }
    }
    out
}

/// Whether a desktop file id is plain enough to write into the list.
pub fn desktop_id_ok(id: &str) -> bool {
    id.ends_with(".desktop")
        && id.len() <= 120
        && id
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_' | '+'))
}

/// `text` with `mime`'s default set to `desktop` (first in its list, the rest kept after it), under `[Default Applications]`. Everything else in the
/// file, comments included, is left as it was.
pub fn set_default(text: &str, mime: &str, desktop: &str) -> String {
    let section = "[Default Applications]";
    let mut lines: Vec<String> = text.lines().map(String::from).collect();
    let start = lines.iter().position(|l| l.trim() == section);
    let Some(start) = start else {
        let mut out = text.trim_end().to_string();
        if !out.is_empty() {
            out.push_str("\n\n");
        }
        out.push_str(&format!("{section}\n{mime}={desktop};\n"));
        return out;
    };
    let end = lines[start + 1..]
        .iter()
        .position(|l| l.trim().starts_with('['))
        .map_or(lines.len(), |i| start + 1 + i);
    let existing = (start + 1..end).find(|&i| {
        lines[i]
            .trim()
            .split_once('=')
            .is_some_and(|(m, _)| m.trim() == mime)
    });
    match existing {
        Some(i) => {
            let rest: Vec<String> = lines[i]
                .split_once('=')
                .map(|(_, apps)| {
                    apps.split(';')
                        .map(str::trim)
                        .filter(|a| !a.is_empty() && *a != desktop)
                        .map(String::from)
                        .collect()
                })
                .unwrap_or_default();
            let mut all = vec![desktop.to_string()];
            all.extend(rest);
            lines[i] = format!("{mime}={};", all.join(";"));
        }
        None => lines.insert(end, format!("{mime}={desktop};")),
    }
    let mut out = lines.join("\n");
    out.push('\n');
    out
}

fn data_dirs() -> Vec<PathBuf> {
    // A test can say exactly which applications exist (the toolkit still needs the real data directories, so it cannot do this with XDG_DATA_DIRS).
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

fn config_home() -> Option<PathBuf> {
    std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))
}

pub fn mimeapps_path() -> Option<PathBuf> {
    config_home().map(|c| c.join("mimeapps.list"))
}

/// What is known about the applications on this computer.
pub struct Apps {
    /// For each type, the desktop ids that say they open it, from `mimeinfo.cache`, in the system's order.
    pub openers: BTreeMap<String, Vec<String>>,
    /// The person's and the system's defaults, most specific file first.
    pub defaults: Vec<BTreeMap<String, Vec<String>>>,
    dirs: Vec<PathBuf>,
}

impl Apps {
    pub fn read() -> Apps {
        let dirs = data_dirs();
        let mut openers: BTreeMap<String, Vec<String>> = BTreeMap::new();
        for dir in dirs.iter().rev() {
            let cache = std::fs::read_to_string(dir.join("applications/mimeinfo.cache"))
                .unwrap_or_default();
            for (mime, apps) in parse_section(&cache, "MIME Cache") {
                let list = openers.entry(mime).or_default();
                for app in apps {
                    if !list.contains(&app) {
                        list.push(app);
                    }
                }
            }
        }
        let mut defaults = Vec::new();
        let mut files: Vec<PathBuf> = mimeapps_path().into_iter().collect();
        files.extend(dirs.iter().map(|d| d.join("applications/mimeapps.list")));
        for file in files {
            if let Ok(text) = std::fs::read_to_string(file) {
                defaults.push(parse_section(&text, "Default Applications"));
            }
        }
        Apps {
            openers,
            defaults,
            dirs,
        }
    }

    /// The program that opens `mime` now: the first default that names an application that is installed, else the first opener.
    pub fn current(&self, mime: &str) -> Option<String> {
        let candidates = self.openers.get(mime);
        self.defaults
            .iter()
            .filter_map(|d| d.get(mime))
            .flatten()
            .find(|app| self.name_of(app).is_some())
            .cloned()
            .or_else(|| {
                candidates.and_then(|c| c.iter().find(|a| self.name_of(a).is_some()).cloned())
            })
    }

    /// What it is called, or `None` when the file is missing, hidden or not for showing.
    pub fn name_of(&self, desktop: &str) -> Option<String> {
        if !desktop_id_ok(desktop) {
            return None;
        }
        for dir in &self.dirs {
            let Ok(text) = std::fs::read_to_string(dir.join("applications").join(desktop)) else {
                continue;
            };
            return entry_name(&text);
        }
        None
    }
}

/// `Name=` of a `[Desktop Entry]`, unless it is `NoDisplay` or `Hidden`.
pub fn entry_name(text: &str) -> Option<String> {
    let mut inside = false;
    let (mut name, mut hidden) = (None, false);
    for line in text.lines() {
        let line = line.trim();
        if let Some(section) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            inside = section == "Desktop Entry";
            continue;
        }
        if !inside {
            continue;
        }
        if let Some(value) = line.strip_prefix("Name=") {
            name.get_or_insert_with(|| {
                value
                    .chars()
                    .filter(|c| !c.is_control())
                    .take(60)
                    .collect::<String>()
            });
        } else if matches!(line, "NoDisplay=true" | "Hidden=true") {
            hidden = true;
        }
    }
    if hidden {
        None
    } else {
        name.filter(|n| !n.is_empty())
    }
}

pub fn rows(apps: &Apps) -> Vec<RowView> {
    KINDS
        .iter()
        .filter_map(|kind| {
            let primary = kind.mimes[0];
            let ids = apps.openers.get(primary)?;
            let mut options: Vec<Choice> = ids
                .iter()
                .filter_map(|id| {
                    apps.name_of(id).map(|name| Choice {
                        value: id.clone(),
                        label: name,
                    })
                })
                .collect();
            if options.is_empty() {
                return None;
            }
            let current = apps.current(primary).unwrap_or_default();
            if !current.is_empty() && !options.iter().any(|o| o.value == current) {
                options.push(Choice {
                    value: current.clone(),
                    label: current.clone(),
                });
            }
            Some(RowView {
                id: format!("default:{}", kind.id),
                title: kind.title.into(),
                group: "Open with".into(),
                description: String::new(),
                overridden: false,
                control: Control::Choice { options, current },
            })
        })
        .collect()
}

/// The types to write for a choice: every type of the kind that the program says it opens, and the first one always.
pub fn plan(apps: &Apps, kind_id: &str, desktop: &str) -> Result<Vec<&'static str>, String> {
    let kind = KINDS
        .iter()
        .find(|k| k.id == kind_id)
        .ok_or("unknown kind of file")?;
    if !desktop_id_ok(desktop) {
        return Err("that is not an application".into());
    }
    let offered = apps
        .openers
        .get(kind.mimes[0])
        .is_some_and(|ids| ids.iter().any(|id| id == desktop));
    if !offered || apps.name_of(desktop).is_none() {
        return Err("that application does not open this kind of file".into());
    }
    let mut types = vec![kind.mimes[0]];
    for mime in &kind.mimes[1..] {
        if apps
            .openers
            .get(*mime)
            .is_some_and(|ids| ids.iter().any(|id| id == desktop))
        {
            types.push(mime);
        }
    }
    Ok(types)
}

/// Sets the choice in the person's `mimeapps.list`, atomically, and returns the rows as they now are.
pub fn choose(kind_id: &str, desktop: &str) -> Result<(), String> {
    let apps = Apps::read();
    let types = plan(&apps, kind_id, desktop)?;
    let path = mimeapps_path().ok_or("no folder for the settings")?;
    let mut text = std::fs::read_to_string(&path).unwrap_or_default();
    for mime in types {
        text = set_default(&text, mime, desktop);
    }
    write_atomic(&path, &text)
}

fn write_atomic(path: &Path, text: &str) -> Result<(), String> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let temporary = path.with_extension(format!("{}.tmp", std::process::id()));
    std::fs::write(&temporary, text).map_err(|e| e.to_string())?;
    std::fs::rename(&temporary, path).map_err(|e| e.to_string())
}

pub fn parse_change(id: &str, change: &Value) -> Result<(String, String), String> {
    let kind = id.strip_prefix("default:").ok_or("unknown change")?;
    if change.get("kind").and_then(Value::as_str) != Some("choice") {
        return Err("unknown change".into());
    }
    let desktop = change
        .get("value")
        .and_then(Value::as_str)
        .ok_or("not text")?;
    Ok((kind.to_string(), desktop.to_string()))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture() -> (tempdir_like::Dir, Apps) {
        let dir = tempdir_like::Dir::new();
        let apps_dir = dir.path().join("share/applications");
        std::fs::create_dir_all(&apps_dir).unwrap();
        std::fs::write(
            apps_dir.join("fox.desktop"),
            "[Desktop Entry]\nName=Fox\nMimeType=x-scheme-handler/https;text/html;\n",
        )
        .unwrap();
        std::fs::write(
            apps_dir.join("owl.desktop"),
            "[Desktop Entry]\nName=Owl\nMimeType=x-scheme-handler/https;\n",
        )
        .unwrap();
        std::fs::write(
            apps_dir.join("ghost.desktop"),
            "[Desktop Entry]\nName=Ghost\nNoDisplay=true\n",
        )
        .unwrap();
        std::fs::write(
            apps_dir.join("mimeinfo.cache"),
            "[MIME Cache]\nx-scheme-handler/https=ghost.desktop;fox.desktop;owl.desktop;\ntext/html=fox.desktop;\n",
        )
        .unwrap();
        let apps = Apps {
            openers: parse_section(
                &std::fs::read_to_string(apps_dir.join("mimeinfo.cache")).unwrap(),
                "MIME Cache",
            ),
            defaults: vec![],
            dirs: vec![dir.path().join("share")],
        };
        (dir, apps)
    }

    mod tempdir_like {
        pub struct Dir(std::path::PathBuf);
        impl Dir {
            pub fn new() -> Dir {
                let path = std::env::temp_dir().join(format!(
                    "helix-defaults-{}-{}",
                    std::process::id(),
                    std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap()
                        .as_nanos()
                ));
                std::fs::create_dir_all(&path).unwrap();
                Dir(path)
            }
            pub fn path(&self) -> &std::path::Path {
                &self.0
            }
        }
        impl Drop for Dir {
            fn drop(&mut self) {
                let _ = std::fs::remove_dir_all(&self.0);
            }
        }
    }

    #[test]
    fn the_default_is_set_first_and_everything_else_in_the_file_is_kept() {
        let text = "# mine\n[Added Associations]\ntext/plain=a.desktop;\n\n[Default Applications]\nx-scheme-handler/https=old.desktop;fox.desktop;\nimage/png=v.desktop;\n";
        let out = set_default(text, "x-scheme-handler/https", "fox.desktop");
        assert!(
            out.contains("x-scheme-handler/https=fox.desktop;old.desktop;"),
            "{out}"
        );
        assert!(
            out.contains("# mine")
                && out.contains("[Added Associations]\ntext/plain=a.desktop;")
                && out.contains("image/png=v.desktop;")
        );
        let added = set_default(text, "text/html", "fox.desktop");
        assert!(
            added.contains("[Default Applications]\nx-scheme-handler/https")
                && added.contains("text/html=fox.desktop;"),
            "{added}"
        );
        assert!(set_default("", "text/html", "fox.desktop")
            .contains("[Default Applications]\ntext/html=fox.desktop;"));
        let both = set_default(
            &set_default("", "text/html", "a.desktop"),
            "text/html",
            "b.desktop",
        );
        assert_eq!(both.matches("text/html=").count(), 1);
    }

    #[test]
    fn only_applications_that_say_they_open_the_type_are_offered_and_hidden_ones_are_not() {
        let (_dir, apps) = fixture();
        let shown = rows(&apps);
        let web = shown.iter().find(|r| r.id == "default:web").unwrap();
        match &web.control {
            Control::Choice { options, current } => {
                assert_eq!(
                    options.iter().map(|o| o.label.as_str()).collect::<Vec<_>>(),
                    ["Fox", "Owl"],
                    "the hidden one is left out"
                );
                assert_eq!(
                    current, "fox.desktop",
                    "the first usable opener when nothing is chosen"
                );
            }
            _ => panic!("a choice"),
        }
        assert!(
            !shown.iter().any(|r| r.id == "default:mail"),
            "no row for a kind nothing opens"
        );
    }

    #[test]
    fn a_choice_writes_the_types_the_program_opens() {
        let (_dir, apps) = fixture();
        assert_eq!(
            plan(&apps, "web", "fox.desktop").unwrap(),
            ["x-scheme-handler/https", "text/html"]
        );
        assert_eq!(
            plan(&apps, "web", "owl.desktop").unwrap(),
            ["x-scheme-handler/https"]
        );
        assert!(plan(&apps, "web", "ghost.desktop").is_err(), "hidden");
        assert!(plan(&apps, "web", "../../evil.desktop").is_err());
        assert!(
            plan(&apps, "mail", "fox.desktop").is_err(),
            "not a mail program"
        );
        assert!(plan(&apps, "nonsense", "fox.desktop").is_err());
    }

    #[test]
    fn a_change_names_a_kind_and_a_program() {
        let ok = serde_json::json!({"kind": "choice", "value": "fox.desktop"});
        assert_eq!(
            parse_change("default:web", &ok),
            Ok(("web".into(), "fox.desktop".into()))
        );
        assert!(parse_change("hostname", &ok).is_err());
        assert!(parse_change(
            "default:web",
            &serde_json::json!({"kind": "switch", "value": true})
        )
        .is_err());
        assert_eq!(
            entry_name("[Desktop Entry]\nName=Fox\n[Desktop Action x]\nName=Other\n").as_deref(),
            Some("Fox")
        );
    }
}
