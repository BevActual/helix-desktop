//! Desktop actions searchable from the launcher with an `act` prefix (and matching keywords).

const PREFIX: &str = "act";

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DesktopAction {
    pub id: &'static str,
    pub name: &'static str,
    pub detail: &'static str,
    pub keywords: &'static [&'static str],
}

const ACTIONS: &[DesktopAction] = &[
    DesktopAction {
        id: "helix:desktop:agent-desk",
        name: "Show agent workspace",
        detail: "Where agents' windows open on your desktop",
        keywords: &["agent", "desk", "mobius", "tools"],
    },
    DesktopAction {
        id: "helix:desktop:notifications",
        name: "Open Notifications",
        detail: "Notification shade",
        keywords: &["notifications", "shade", "bell", "messages"],
    },
    DesktopAction {
        id: "helix:desktop:control-center",
        name: "Open Control Center",
        detail: "Sound, network, and quick settings",
        keywords: &["control", "center", "settings", "quick", "wifi", "volume"],
    },
    DesktopAction {
        id: "helix:desktop:launcher",
        name: "Open Apps",
        detail: "Application launcher",
        keywords: &["apps", "launcher", "applications", "run"],
    },
    DesktopAction {
        id: "helix:desktop:assistant",
        name: "Ask Mobius",
        detail: "It answers, or does it on the desktop",
        keywords: &["mobius", "assistant", "act", "ai"],
    },
    DesktopAction {
        id: "helix:desktop:agents",
        name: "Open Agents",
        detail: "Grants and agent activity",
        keywords: &["agents", "grants", "permissions"],
    },
    DesktopAction {
        id: "helix:desktop:privacy-agents",
        name: "Privacy & agents",
        detail: "Settings → agents and compositor",
        keywords: &["privacy", "agents", "grants", "compositor"],
    },
    DesktopAction {
        id: "helix:desktop:settings",
        name: "Open Settings",
        detail: "Helix Settings",
        keywords: &["settings", "preferences", "configure"],
    },
    DesktopAction {
        id: "helix:desktop:mobius",
        name: "Open Mobius",
        detail: "Full Mobius window",
        keywords: &["mobius", "chat", "ai", "assistant"],
    },
    DesktopAction {
        id: "helix:desktop:software",
        name: "Open Helix Software",
        detail: "Apps, updates, and firmware",
        keywords: &["software", "apps", "flatpak", "updates", "store"],
    },
    DesktopAction {
        id: "helix:desktop:system-monitor",
        name: "Open System Monitor",
        detail: "Processes and resources",
        keywords: &["monitor", "system", "cpu", "memory", "processes"],
    },
    DesktopAction {
        id: "helix:desktop:lock-screen",
        name: "Lock screen",
        detail: "Lock this session",
        keywords: &["lock", "secure", "away"],
    },
    DesktopAction {
        id: "helix:desktop:night-light",
        name: "Toggle Night Light",
        detail: "Warmer screen colours",
        keywords: &["night", "light", "blue", "warm", "evening"],
    },
    DesktopAction {
        id: "helix:desktop:dark-theme",
        name: "Toggle dark theme",
        detail: "Switch light and dark appearance",
        keywords: &["dark", "light", "theme", "appearance"],
    },
    DesktopAction {
        id: "helix:desktop:dnd",
        name: "Toggle Do Not Disturb",
        detail: "Silence notification banners",
        keywords: &["dnd", "focus", "quiet", "notifications"],
    },
    DesktopAction {
        id: "helix:desktop:workspace-next",
        name: "Next workspace",
        detail: "Switch workspace forward",
        keywords: &["workspace", "desktop", "next"],
    },
    DesktopAction {
        id: "helix:desktop:workspace-prev",
        name: "Previous workspace",
        detail: "Switch workspace back",
        keywords: &["workspace", "desktop", "previous", "prev"],
    },
    DesktopAction {
        id: "helix:desktop:close-window",
        name: "Close focused window",
        detail: "Close the window that has focus",
        keywords: &["close", "window", "kill"],
    },
    DesktopAction {
        id: "helix:desktop:bar-hide",
        name: "Hide top bar",
        detail: "Hide the bar and release the top gap",
        keywords: &["bar", "hide", "chrome"],
    },
    DesktopAction {
        id: "helix:desktop:bar-show",
        name: "Show top bar",
        detail: "Show the bar and reserve the top gap",
        keywords: &["bar", "show", "chrome"],
    },
    DesktopAction {
        id: "helix:desktop:bar-toggle",
        name: "Toggle top bar",
        detail: "Show or hide the bar",
        keywords: &["bar", "toggle", "chrome"],
    },
];

fn score(query: &str, action: &DesktopAction) -> u32 {
    let name = action.name.to_lowercase();
    if name == query {
        return 100;
    }
    if name.starts_with(query) {
        return 80;
    }
    if action
        .keywords
        .iter()
        .any(|k| *k == query || k.starts_with(query))
    {
        return 60;
    }
    if name.contains(query) {
        return 40;
    }
    if action.keywords.iter().any(|k| k.contains(query)) {
        return 30;
    }
    0
}

/// Strip a leading `act` prefix (optional colon or space). Returns `(in_act_mode, needle)`.
pub fn parse_act_query(raw: &str) -> (bool, String) {
    let trimmed = raw.trim();
    let lower = trimmed.to_lowercase();
    if lower == PREFIX {
        return (true, String::new());
    }
    if lower.starts_with("act ") {
        return (true, trimmed[4..].trim().to_lowercase());
    }
    if lower.starts_with("act:") {
        return (true, trimmed[4..].trim().to_lowercase());
    }
    (false, trimmed.to_lowercase())
}

/// Launcher hits when the query uses the `act` prefix or matches action keywords strongly.
pub fn search(raw: &str, limit: usize) -> Vec<&'static DesktopAction> {
    let (act_mode, needle) = parse_act_query(raw);
    if !act_mode && !needle.is_empty() {
        // Global search: surface a few actions when the query matches without the prefix.
        let mut hits: Vec<(u32, usize)> = ACTIONS
            .iter()
            .enumerate()
            .filter_map(|(i, a)| {
                let s = score(&needle, a);
                (s >= 55).then_some((s, i))
            })
            .collect();
        hits.sort_by(|a, b| b.0.cmp(&a.0).then(a.1.cmp(&b.1)));
        return hits
            .into_iter()
            .take(limit.min(3))
            .map(|(_, i)| &ACTIONS[i])
            .collect();
    }
    if !act_mode {
        return Vec::new();
    }
    if needle.is_empty() {
        return ACTIONS.iter().take(limit).collect();
    }
    let mut hits: Vec<(u32, usize)> = ACTIONS
        .iter()
        .enumerate()
        .filter_map(|(i, a)| {
            let s = score(&needle, a);
            (s > 0).then_some((s, i))
        })
        .collect();
    hits.sort_by(|a, b| b.0.cmp(&a.0).then(a.1.cmp(&b.1)));
    hits.into_iter()
        .take(limit)
        .map(|(_, i)| &ACTIONS[i])
        .collect()
}

pub fn get(id: &str) -> Option<&'static DesktopAction> {
    ACTIONS.iter().find(|a| a.id == id)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_act_prefix() {
        assert_eq!(parse_act_query("act"), (true, String::new()));
        assert_eq!(parse_act_query("act: dnd"), (true, "dnd".into()));
        assert_eq!(parse_act_query("firefox"), (false, "firefox".into()));
    }

    #[test]
    fn act_prefix_lists_actions() {
        assert!(search("firefox", 8).is_empty());
        assert!(search("act", 8).len() >= 5);
        assert_eq!(search("act agent", 3)[0].id, "helix:desktop:agent-desk");
        assert_eq!(
            search("notifications", 1)[0].id,
            "helix:desktop:notifications"
        );
        assert!(search("act hide", 5)
            .iter()
            .any(|a| a.id == "helix:desktop:bar-hide"));
    }
}
