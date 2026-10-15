//! What the greeter may offer: login names and sessions, read from the system. The page only ever sends back an id it was given.

use serde::Serialize;
use std::path::Path;

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct User {
    pub username: String,
    pub name: String,
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct Session {
    pub id: String,
    pub name: String,
}

/// Regular accounts from a passwd file: uid 1000..60000 with a real shell.
pub fn users_from_passwd(passwd: &str) -> Vec<User> {
    passwd
        .lines()
        .filter_map(|line| {
            let fields: Vec<&str> = line.split(':').collect();
            if fields.len() < 7 {
                return None;
            }
            let uid: u32 = fields[2].parse().ok()?;
            let shell = fields[6];
            if !(1000..60000).contains(&uid)
                || shell.ends_with("nologin")
                || shell.ends_with("/false")
                || shell.is_empty()
            {
                return None;
            }
            let name = fields[4].split(',').next().unwrap_or("").trim();
            Some(User {
                username: fields[0].to_string(),
                name: if name.is_empty() {
                    fields[0].to_string()
                } else {
                    name.to_string()
                },
            })
        })
        .collect()
}

/// A session description: its display name and the command to run (split on spaces; no shell is involved).
pub fn parse_session(file_stem: &str, text: &str) -> Option<(Session, Vec<String>)> {
    let mut name = None;
    let mut exec = None;
    let mut in_entry = false;
    for line in text.lines() {
        let line = line.trim();
        if line.starts_with('[') {
            in_entry = line == "[Desktop Entry]";
        } else if in_entry {
            if let Some(value) = line.strip_prefix("Name=") {
                name.get_or_insert(value.trim().to_string());
            } else if let Some(value) = line.strip_prefix("Exec=") {
                exec.get_or_insert(value.trim().to_string());
            } else if line == "Hidden=true" || line == "NoDisplay=true" {
                return None;
            }
        }
    }
    let cmd: Vec<String> = exec?
        .split_whitespace()
        .filter(|word| !word.starts_with('%'))
        .map(str::to_string)
        .collect();
    if cmd.is_empty() {
        return None;
    }
    Some((
        Session {
            id: file_stem.to_string(),
            name: name.unwrap_or_else(|| file_stem.to_string()),
        },
        cmd,
    ))
}

pub fn sessions_in(dir: &Path) -> Vec<(Session, Vec<String>)> {
    let mut found: Vec<_> = std::fs::read_dir(dir)
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|entry| {
            let path = entry.path();
            if path.extension()? != "desktop" {
                return None;
            }
            let stem = path.file_stem()?.to_str()?.to_string();
            parse_session(&stem, &std::fs::read_to_string(&path).ok()?)
        })
        .collect();
    found.sort_by(|a, b| a.0.name.cmp(&b.0.name));
    found
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_regular_login_accounts_are_listed() {
        let passwd = "root:x:0:0:root:/root:/bin/bash\nada:x:1000:1000:Ada L,,,:/home/ada:/bin/bash\nsvc:x:1001:1001::/var/svc:/usr/sbin/nologin\nnobody:x:65534:65534::/:/bin/false\n";
        assert_eq!(
            users_from_passwd(passwd),
            vec![User {
                username: "ada".into(),
                name: "Ada L".into()
            }]
        );
    }

    #[test]
    fn a_session_file_gives_a_name_and_a_command_without_a_shell() {
        let text = "[Desktop Entry]\nName=Helix\nExec=niri-session --flag %u\n";
        let (session, cmd) = parse_session("helix", text).unwrap();
        assert_eq!(
            session,
            Session {
                id: "helix".into(),
                name: "Helix".into()
            }
        );
        assert_eq!(cmd, vec!["niri-session", "--flag"]);
        assert!(parse_session(
            "x",
            "[Desktop Entry]\nName=Hidden\nNoDisplay=true\nExec=a\n"
        )
        .is_none());
    }
}
