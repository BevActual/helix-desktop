//! The key binds in the compositor's config, as a read-only list for Settings. It reads only single-line binds (`Mod+D { spawn "..."; }`),
//! shows each chord as separate keys, groups them by what they do, and flags a chord that two files bind (the compositor takes one of them
//! and the person cannot tell which). Nothing here writes: changing a bind is a config edit that belongs to a later, reviewed step.

use std::collections::BTreeMap;

use serde::Serialize;

use crate::display::sanitize_for_display;

const MAX_BINDS: usize = 400;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Bind {
    pub chord: String,
    pub title: String,
    pub command: String,
    /// The file the bind came from (its name only).
    pub source: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ShortcutRow {
    pub keys: Vec<String>,
    pub title: String,
    pub conflict: bool,
    pub source: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct ShortcutGroup {
    pub name: &'static str,
    pub rows: Vec<ShortcutRow>,
}

/// Top-level config nodes that look like a bind on one line (`layout { gaps 8; }`).
const NOT_BINDS: &[&str] = &[
    "layout", "input", "output", "binds", "animations", "gestures", "overview", "cursor", "window-rule", "layer-rule", "environment",
    "debug", "workspace", "switch-events", "clipboard", "hotkey-overlay", "xwayland-satellite", "prefer-no-csd", "focus-ring", "border",
    "shadow", "struts", "tab-indicator", "insert-hint", "background-effect", "recent-windows", "config-notification",
];

fn is_chord(token: &str) -> bool {
    !token.is_empty()
        && !NOT_BINDS.contains(&token)
        && token.len() <= 64
        && token.chars().all(|c| c.is_ascii_alphanumeric() || c == '+' || c == '_')
        && !token.starts_with('+')
        && !token.ends_with('+')
}

fn quoted_values(text: &str) -> Vec<String> {
    text.split('"').skip(1).step_by(2).map(str::to_string).collect()
}

/// The binds on single lines of one config file. `source` is only used for display.
pub fn parse(text: &str, source: &str) -> Vec<Bind> {
    let mut binds = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if line.starts_with("//") || !line.ends_with('}') {
            continue;
        }
        let (Some(open), Some(close)) = (line.find('{'), line.rfind('}')) else { continue };
        if open >= close {
            continue;
        }
        let head = &line[..open];
        let body = line[open + 1..close].trim().trim_end_matches(';').trim();
        let Some(chord) = head.split_whitespace().next().filter(|t| is_chord(t)) else { continue };
        // The rest of the head must be properties (`name=value`) only, so a block like `layout { ... }` is not mistaken for a bind.
        let props = head[head.find(chord).unwrap_or(0) + chord.len()..].trim();
        let title = props
            .split("hotkey-overlay-title=")
            .nth(1)
            .and_then(|rest| quoted_values(rest).into_iter().next())
            .unwrap_or_default();
        let without_title = props.replace(&format!("hotkey-overlay-title=\"{title}\""), "");
        if !without_title.split_whitespace().all(|word| word.contains('=')) {
            continue;
        }
        if body.is_empty() || body.contains('{') {
            continue;
        }
        let command = if let Some(rest) = body.strip_prefix("spawn-sh") {
            quoted_values(rest).join(" ")
        } else if let Some(rest) = body.strip_prefix("spawn") {
            quoted_values(rest).join(" ")
        } else {
            body.to_string()
        };
        binds.push(Bind {
            chord: chord.to_string(),
            title: sanitize_for_display(&title, 80),
            command: sanitize_for_display(&command, 200),
            source: sanitize_for_display(source, 60),
        });
        if binds.len() >= MAX_BINDS {
            break;
        }
    }
    binds
}

/// A chord as keys a person recognises: `Mod+Shift+Comma` → `Super`, `Shift`, `,`.
pub fn keys_of(chord: &str) -> Vec<String> {
    chord
        .split('+')
        .map(|part| match part {
            "Mod" | "Super" | "Win" => "Super".to_string(),
            "Ctrl" | "Control" => "Ctrl".to_string(),
            "Comma" => ",".to_string(),
            "Period" => ".".to_string(),
            "Minus" => "-".to_string(),
            "Equal" => "=".to_string(),
            "Return" => "Return".to_string(),
            other => other.strip_prefix("XF86").unwrap_or(other).to_string(),
        })
        .collect()
}

/// What a bind is called: its own title, or what its command is.
fn title_of(bind: &Bind) -> String {
    if !bind.title.is_empty() {
        return bind.title.clone();
    }
    match bind.command.split_whitespace().last() {
        Some(last) if bind.command.contains("helix-panel") => {
            let mut text = last.replace('-', " ");
            if let Some(first) = text.get_mut(0..1) {
                first.make_ascii_uppercase();
            }
            text
        }
        _ if !bind.command.contains(' ') && bind.command.contains('-') => {
            let mut text = bind.command.replace('-', " ");
            if let Some(first) = text.get_mut(0..1) {
                first.make_ascii_uppercase();
            }
            text
        }
        _ => bind.command.clone(),
    }
}

fn group_of(bind: &Bind) -> &'static str {
    if bind.chord.starts_with("XF86") {
        "Media keys"
    } else if bind.command.contains("helix-panel") {
        "Helix"
    } else {
        "Windows and apps"
    }
}

/// Two chords are the same when they are the same keys: modifier order and the `Mod`/`Super` spelling do not matter.
fn identity(chord: &str) -> String {
    let mut parts: Vec<String> = keys_of(chord).into_iter().map(|p| p.to_lowercase()).collect();
    if let Some(key) = parts.pop() {
        parts.sort();
        parts.push(key);
    }
    parts.join("+")
}

pub fn view(binds: &[Bind]) -> Vec<ShortcutGroup> {
    let mut seen: BTreeMap<String, usize> = BTreeMap::new();
    for bind in binds {
        *seen.entry(identity(&bind.chord)).or_default() += 1;
    }
    let mut groups: Vec<ShortcutGroup> = ["Helix", "Windows and apps", "Media keys"]
        .into_iter()
        .map(|name| ShortcutGroup { name, rows: Vec::new() })
        .collect();
    for bind in binds {
        let name = group_of(bind);
        let row = ShortcutRow {
            keys: keys_of(&bind.chord),
            title: title_of(bind),
            conflict: seen.get(&identity(&bind.chord)).copied().unwrap_or(0) > 1,
            source: bind.source.clone(),
        };
        if let Some(group) = groups.iter_mut().find(|g| g.name == name) {
            group.rows.push(row);
        }
    }
    groups.retain(|group| !group.rows.is_empty());
    groups
}

#[cfg(test)]
mod tests {
    use super::*;

    const CONFIG: &str = r#"
binds {
    // a comment { spawn "x"; }
    Mod+T hotkey-overlay-title="Open a Terminal" { spawn "warp-terminal"; }
    Mod+D hotkey-overlay-title="Open Launcher" { spawn "/home/a/.local/bin/helix-panel" "launcher"; }
    Mod+S { spawn "/home/a/.local/bin/helix-panel" "control-center"; }
    Mod+C repeat=false hotkey-overlay-title="Copy" { spawn-sh "wtype -s 50 -M ctrl -k Insert"; }
    XF86AudioRaiseVolume allow-when-locked=true { spawn "/x/helix-panel" "volume-up"; }
    Mod+Shift+C hotkey-overlay-title="Center column" { center-column; }
    layout { gaps 8; }
    Mod+Q { close-window; }
    Mod+W {
        close-window;
    }
}
"#;

    #[test]
    fn single_line_binds_are_read_with_their_titles_and_commands() {
        let binds = parse(CONFIG, "config.kdl");
        let chords: Vec<&str> = binds.iter().map(|b| b.chord.as_str()).collect();
        assert_eq!(chords, ["Mod+T", "Mod+D", "Mod+S", "Mod+C", "XF86AudioRaiseVolume", "Mod+Shift+C", "Mod+Q"]);
        assert_eq!(binds[0].title, "Open a Terminal");
        assert_eq!(binds[0].command, "warp-terminal");
        assert_eq!(binds[1].command, "/home/a/.local/bin/helix-panel launcher");
        assert_eq!(binds[3].command, "wtype -s 50 -M ctrl -k Insert");
        assert_eq!(binds[5].command, "center-column");
        assert!(binds.iter().all(|b| b.source == "config.kdl"));
    }

    #[test]
    fn blocks_comments_and_odd_lines_are_not_binds() {
        assert!(parse("layout { gaps 8; }", "x").is_empty());
        assert!(parse("// Mod+T { spawn \"a\"; }", "x").is_empty());
        assert!(parse("Mod+T { }", "x").is_empty(), "an empty action is not a bind");
        assert!(parse("Mod+T { a { b } }", "x").is_empty(), "nested blocks are not read");
        assert!(parse("Mod+T something odd { spawn \"a\"; }", "x").is_empty(), "words that are not properties");
        assert!(parse("++ { spawn \"a\"; }", "x").is_empty());
    }

    #[test]
    fn chords_become_keys() {
        assert_eq!(keys_of("Mod+Shift+Comma"), ["Super", "Shift", ","]);
        assert_eq!(keys_of("Ctrl+Alt+Delete"), ["Ctrl", "Alt", "Delete"]);
        assert_eq!(keys_of("XF86AudioMute"), ["AudioMute"]);
    }

    #[test]
    fn titles_fall_back_to_what_the_command_is() {
        let binds = parse(CONFIG, "config.kdl");
        let groups = view(&binds);
        let helix = groups.iter().find(|g| g.name == "Helix").unwrap();
        assert_eq!(helix.rows[0].title, "Open Launcher");
        assert_eq!(helix.rows[1].title, "Control center", "no title: the panel's name, spaced and capitalised");
        let apps = groups.iter().find(|g| g.name == "Windows and apps").unwrap();
        assert!(apps.rows.iter().any(|r| r.title == "Close window"), "an action with no title shows as it is written") ;
    }

    #[test]
    fn groups_split_helix_media_and_the_rest_and_empty_ones_are_dropped() {
        let groups = view(&parse(CONFIG, "c"));
        let names: Vec<&str> = groups.iter().map(|g| g.name).collect();
        assert_eq!(names, ["Helix", "Windows and apps", "Media keys"]);
        assert_eq!(groups[0].rows.len(), 2);
        assert_eq!(groups[2].rows.len(), 1);
        assert!(view(&[]).is_empty());
    }

    #[test]
    fn a_chord_bound_twice_is_flagged_however_it_is_spelled() {
        let a = parse("Mod+D { spawn \"a\"; }\nMod+Shift+X { spawn \"b\"; }\nAlt+Q { spawn \"c\"; }", "one.kdl");
        let b = parse("Super+D { spawn \"z\"; }\nShift+Mod+X { spawn \"y\"; }\nAlt+W { spawn \"d\"; }", "two.kdl");
        let all: Vec<Bind> = a.into_iter().chain(b).collect();
        let rows: Vec<ShortcutRow> = view(&all).into_iter().flat_map(|g| g.rows).collect();
        let flagged: Vec<usize> = rows.iter().enumerate().filter(|(_, r)| r.conflict).map(|(i, _)| i).collect();
        assert_eq!(flagged.len(), 4, "both ends of both clashes");
        assert!(rows.iter().filter(|r| !r.conflict).all(|r| r.keys[0] == "Alt"), "Alt+Q and Alt+W are not clashes");
        assert!(rows.iter().any(|r| r.conflict && r.source == "two.kdl"));
    }

    #[test]
    fn the_last_key_stays_last_when_modifiers_are_compared() {
        assert_ne!(identity("Mod+Shift+C"), identity("Mod+C+Shift"));
        assert_eq!(identity("Mod+Shift+C"), identity("Shift+Super+C"));
    }

    #[test]
    fn the_number_of_binds_is_bounded() {
        let text: String = (0..1000).map(|i| format!("Mod+F{i} {{ spawn \"a\"; }}\n")).collect();
        assert_eq!(parse(&text, "x").len(), MAX_BINDS);
    }

    #[test]
    fn displayed_text_has_no_control_characters() {
        let binds = parse("Mod+T hotkey-overlay-title=\"A\u{202e}B\" { spawn \"c\u{7}d\"; }", "f\u{202e}.kdl");
        let b = &binds[0];
        for t in [&b.title, &b.command, &b.source] {
            assert!(!t.contains('\u{202e}') && !t.contains('\u{7}'), "{t:?}");
        }
    }
}
