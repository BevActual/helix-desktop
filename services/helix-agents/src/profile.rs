//! How much an agent may do. Four profiles, from asking to trusting; the box and the agent's own prompts are separate dials.
//!
//! | profile | Helix box | the agent's own permission prompts |
//! |---|---|---|
//! | `ask` (default) | yes | on (the agent asks as it normally does) |
//! | `auto` | yes | off: the agent works freely *inside the box* |
//! | `open` | no | on (the agent as it would run anywhere, but launched, logged and given the desktop tools) |
//! | `danger` | no | off, for a limited time, with a banner |
//!
//! `auto` is the one to live in: the folder is writable, the network works, nothing else of the machine is reachable, so skipping prompts costs little.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Profile {
    Ask,
    Auto,
    Open,
    Danger,
}

impl Profile {
    pub fn parse(word: &str) -> Option<Self> {
        match word {
            "ask" => Some(Self::Ask),
            "auto" => Some(Self::Auto),
            "open" => Some(Self::Open),
            "danger" => Some(Self::Danger),
            _ => None,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Self::Ask => "ask",
            Self::Auto => "auto",
            Self::Open => "open",
            Self::Danger => "danger",
        }
    }

    /// Whether the agent runs inside the Helix sandbox.
    pub fn boxed(self) -> bool {
        matches!(self, Self::Ask | Self::Auto)
    }

    /// Whether the agent's own permission prompts are switched off.
    pub fn skips_prompts(self) -> bool {
        matches!(self, Self::Auto | Self::Danger)
    }

    /// Whether the profile needs a time limit (anything that is both unboxed and unprompted).
    pub fn needs_expiry(self) -> bool {
        self == Self::Danger
    }

    pub fn about(self) -> &'static str {
        match self {
            Self::Ask => "in the Helix box; the agent asks before it acts, as usual",
            Self::Auto => {
                "in the Helix box; the agent works without asking (it can only touch the folder)"
            }
            Self::Open => "no box; the agent asks as usual; launched and logged by Helix",
            Self::Danger => "no box and no prompts, for a limited time",
        }
    }
}

/// `30m`, `2h`, `90s`, or a bare number of minutes.
pub fn parse_duration(text: &str) -> Option<u64> {
    let text = text.trim();
    let (number, unit) = match text.char_indices().find(|(_, c)| !c.is_ascii_digit()) {
        Some((at, _)) => text.split_at(at),
        None => (text, "m"),
    };
    let n: u64 = number.parse().ok()?;
    let seconds = match unit {
        "s" => n,
        "m" | "" => n.checked_mul(60)?,
        "h" => n.checked_mul(3600)?,
        _ => return None,
    };
    (1..=24 * 3600).contains(&seconds).then_some(seconds)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_dials_are_independent() {
        assert!(Profile::Ask.boxed() && !Profile::Ask.skips_prompts());
        assert!(Profile::Auto.boxed() && Profile::Auto.skips_prompts());
        assert!(!Profile::Open.boxed() && !Profile::Open.skips_prompts());
        assert!(
            !Profile::Danger.boxed()
                && Profile::Danger.skips_prompts()
                && Profile::Danger.needs_expiry()
        );
        assert_eq!(Profile::parse("auto"), Some(Profile::Auto));
        assert_eq!(Profile::parse("yolo"), None);
    }

    #[test]
    fn durations() {
        assert_eq!(parse_duration("30m"), Some(1800));
        assert_eq!(parse_duration("2h"), Some(7200));
        assert_eq!(parse_duration("90s"), Some(90));
        assert_eq!(parse_duration("45"), Some(2700));
        assert_eq!(parse_duration("0m"), None);
        assert_eq!(parse_duration("99h"), None);
        assert_eq!(parse_duration("soon"), None);
    }
}
