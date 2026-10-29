//! One place to ask (`docs/design/north-star.md`, "one presence"): whatever the person types to Mobius, Helix decides whether it is a
//! question to answer or something to do on the desktop, so there is one box, not a Talk box and an Act box. This is the first, plain rule:
//! a request that starts (after a polite opening) with a verb of doing is done; everything else is answered. The person can always say
//! "Do it" on an answer, and the decision is shown, so a wrong guess costs one click.

/// Words a request to do something starts with.
const DOING: [&str; 44] = [
    "turn",
    "switch",
    "set",
    "make",
    "open",
    "close",
    "launch",
    "start",
    "stop",
    "quit",
    "enable",
    "disable",
    "mute",
    "unmute",
    "increase",
    "decrease",
    "raise",
    "lower",
    "dim",
    "brighten",
    "show",
    "hide",
    "lock",
    "change",
    "put",
    "move",
    "connect",
    "disconnect",
    "pair",
    "unpair",
    "play",
    "pause",
    "resume",
    "skip",
    "toggle",
    "take",
    "screenshot",
    "maximize",
    "minimise",
    "minimize",
    "fullscreen",
    "go",
    "focus",
    "silence",
];

/// Openings that do not change what is asked.
const POLITE: [&str; 9] = [
    "please",
    "can you",
    "could you",
    "would you",
    "will you",
    "can u",
    "pls",
    "hey mobius",
    "mobius",
];

/// Whether `text` asks Helix to do something on the desktop (rather than to answer).
pub fn wants_doing(text: &str) -> bool {
    let mut rest = text.trim().to_lowercase();
    loop {
        let before = rest.len();
        for opening in POLITE {
            if let Some(after) = rest.strip_prefix(opening) {
                if after.is_empty() || after.starts_with([' ', ',']) {
                    rest = after.trim_start_matches([' ', ',']).to_string();
                }
            }
        }
        if rest.len() == before {
            break;
        }
    }
    let first = rest
        .split(|c: char| !c.is_alphanumeric())
        .next()
        .unwrap_or_default();
    // "Show me how…", "Make sense of…": explanation, not doing.
    if (first == "show" && rest.starts_with("show me how")) || rest.starts_with("make sense") {
        return false;
    }
    DOING.contains(&first)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn requests_to_do_are_done() {
        for text in [
            "turn on night light",
            "Please set the dock to be smaller",
            "could you mute the sound?",
            "Mobius, open Firefox",
            "can you, please, switch to dark mode",
            "lock the screen",
            "take a screenshot",
        ] {
            assert!(wants_doing(text), "{text}");
        }
    }

    #[test]
    fn questions_are_answered() {
        for text in [
            "why is my laptop fan so loud",
            "what is night light?",
            "how do I set up a printer",
            "show me how to use git rebase",
            "explain this error",
            "can you explain closures",
            "",
            "turnip soup recipe",
        ] {
            assert!(!wants_doing(text), "{text}");
        }
    }
}
