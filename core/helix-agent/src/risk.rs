//! Risk tiers and undo classes from `protocols/ipc/msg-risk.toml`. Unknown commands are Blocked (fail closed).
//! A command with no change class is `none`, so a missing row never offers undo.
//! Port of the former C++ `helix::agent::RiskPolicy` (`agent_policy.*`, removed).

use std::collections::BTreeMap;

use toml::Value;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Tier {
    Observe,
    ObservePrivate,
    Act,
    Confirm,
    Blocked,
}

impl Tier {
    pub fn name(self) -> &'static str {
        match self {
            Self::Observe => "observe",
            Self::ObservePrivate => "observe-private",
            Self::Act => "act",
            Self::Confirm => "confirm",
            Self::Blocked => "blocked",
        }
    }

    pub fn parse(name: &str) -> Option<Self> {
        match name {
            "observe" => Some(Self::Observe),
            "observe-private" => Some(Self::ObservePrivate),
            "act" => Some(Self::Act),
            "confirm" => Some(Self::Confirm),
            "blocked" => Some(Self::Blocked),
            _ => None,
        }
    }
}

/// Whether a finished command can be undone. Independent of [`Tier`]: `act` is a grant, not a promise that the change can be put back.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Change {
    /// One short setting. The shell may record the previous value and write it back.
    Restore,
    /// No before and after, and no undo.
    None,
}

impl Change {
    pub fn name(self) -> &'static str {
        match self {
            Self::Restore => "restore",
            Self::None => "none",
        }
    }

    pub fn parse(name: &str) -> Option<Self> {
        match name {
            "restore" => Some(Self::Restore),
            "none" => Some(Self::None),
            _ => None,
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RiskPolicy {
    tiers: BTreeMap<String, Tier>,
    /// Keys are `"command first-arg"` for choice overrides.
    choices: BTreeMap<String, Tier>,
    /// Present only when the file has a `[changes]` table. An empty map means the file did not say, and every command is [`Change::None`].
    changes: BTreeMap<String, Change>,
}

impl RiskPolicy {
    pub fn parse(text: &str) -> Result<Self, String> {
        let table: toml::Table = text
            .parse()
            .map_err(|error| format!("risk policy: {error}"))?;
        let mut policy = Self::default();
        read_section(&table, "tiers", &mut policy.tiers)?;
        read_section(&table, "choices", &mut policy.choices)?;
        if policy.tiers.is_empty() {
            return Err("risk policy: no [tiers] entries".into());
        }
        policy.changes = read_changes(&table, &policy.tiers)?;
        Ok(policy)
    }

    pub fn embedded() -> Self {
        Self::parse(include_str!("../../../protocols/ipc/msg-risk.toml"))
            .expect("bundled msg-risk.toml")
    }

    /// `HELIX_RISK_POLICY` path when set and readable, else [`embedded`].
    pub fn from_env() -> Self {
        let Ok(path) = std::env::var("HELIX_RISK_POLICY") else {
            return Self::embedded();
        };
        if path.is_empty() {
            return Self::embedded();
        }
        let Ok(text) = std::fs::read_to_string(&path) else {
            return Self::embedded();
        };
        Self::parse(&text).unwrap_or_else(|_| Self::embedded())
    }

    pub fn tier_for(&self, command: &str, args: &[String]) -> Tier {
        if let Some(first) = args.first() {
            let key = format!("{command} {first}");
            if let Some(tier) = self.choices.get(&key) {
                return *tier;
            }
        }
        self.tiers.get(command).copied().unwrap_or(Tier::Blocked)
    }

    pub fn knows(&self, command: &str) -> bool {
        self.tiers.contains_key(command)
    }

    pub fn commands(&self) -> Vec<String> {
        self.tiers.keys().cloned().collect()
    }

    /// The undo class. A command the file does not list, and a file with no `[changes]` table, are [`Change::None`].
    pub fn change_for(&self, command: &str) -> Change {
        self.changes.get(command).copied().unwrap_or(Change::None)
    }

    pub fn choice_tiers(&self, command: &str) -> BTreeMap<String, Tier> {
        let prefix = format!("{command} ");
        self.choices
            .iter()
            .filter_map(|(key, tier)| {
                key.strip_prefix(&prefix)
                    .map(|choice| (choice.to_string(), *tier))
            })
            .collect()
    }
}

fn read_section(
    table: &toml::Table,
    section: &str,
    into: &mut BTreeMap<String, Tier>,
) -> Result<(), String> {
    let Some(Value::Table(node)) = table.get(section) else {
        return Ok(());
    };
    for (key, value) in node {
        let Some(name) = value.as_str() else {
            return Err(format!("risk policy: {section}.{key} has an unknown tier"));
        };
        let Some(tier) = Tier::parse(name) else {
            return Err(format!("risk policy: {section}.{key} has an unknown tier"));
        };
        into.insert(key.clone(), tier);
    }
    Ok(())
}

/// When `[changes]` is present it must name every command in `[tiers]`, and nothing else. A missing table leaves the map empty.
fn read_changes(
    table: &toml::Table,
    tiers: &BTreeMap<String, Tier>,
) -> Result<BTreeMap<String, Change>, String> {
    let Some(Value::Table(node)) = table.get("changes") else {
        return Ok(BTreeMap::new());
    };
    let mut changes = BTreeMap::new();
    for (key, value) in node {
        if !tiers.contains_key(key) {
            return Err(format!("risk policy: changes.{key} is not a command"));
        }
        let Some(name) = value.as_str() else {
            return Err(format!("risk policy: changes.{key} has an unknown class"));
        };
        let Some(class) = Change::parse(name) else {
            return Err(format!("risk policy: changes.{key} has an unknown class"));
        };
        changes.insert(key.clone(), class);
    }
    for command in tiers.keys() {
        if !changes.contains_key(command) {
            return Err(format!("risk policy: {command} has no change class"));
        }
    }
    Ok(changes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn from_env_reads_policy_file() {
        let dir = std::env::temp_dir().join(format!("helix-risk-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let file = dir.join("custom.toml");
        std::fs::write(&file, "[tiers]\nonly-cmd = \"observe\"\n").unwrap();
        std::env::set_var("HELIX_RISK_POLICY", &file);
        let policy = RiskPolicy::from_env();
        assert!(policy.knows("only-cmd"));
        assert!(!policy.knows("status"));
        assert_eq!(
            policy.change_for("only-cmd"),
            Change::None,
            "a file with no [changes] table offers no undo"
        );
        std::env::remove_var("HELIX_RISK_POLICY");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn embedded_policy_loads_and_fail_closed() {
        let policy = RiskPolicy::embedded();
        assert!(policy.knows("status"));
        assert_eq!(policy.tier_for("status", &[]), Tier::Observe);
        assert_eq!(policy.tier_for("not-a-real-command", &[]), Tier::Blocked);
        assert_eq!(policy.change_for("volume-set"), Change::Restore);
        assert_eq!(policy.change_for("wifi-toggle"), Change::Restore);
        assert_eq!(policy.change_for("panel-open"), Change::None);
        assert_eq!(policy.change_for("clipboard-copy"), Change::None);
        assert_eq!(policy.change_for("not-a-real-command"), Change::None);
        assert!(policy
            .commands()
            .iter()
            .all(|command| policy.changes.contains_key(command)));
    }

    #[test]
    fn choice_override() {
        let policy = RiskPolicy::embedded();
        assert_eq!(policy.tier_for("session", &[]), Tier::Blocked);
        let lock = policy.tier_for("session", &["lock".into()]);
        assert_ne!(lock, Tier::Blocked, "choices should override session lock");
        assert_eq!(
            policy.change_for("session"),
            Change::None,
            "locking and suspending are not undone"
        );
    }

    #[test]
    fn a_changes_table_must_cover_every_command_and_nothing_else() {
        let missing =
            RiskPolicy::parse("[tiers]\na = \"act\"\nb = \"act\"\n[changes]\na = \"restore\"\n")
                .unwrap_err();
        assert!(missing.contains("b"), "{missing}");
        let extra = RiskPolicy::parse(
            "[tiers]\na = \"act\"\n[changes]\na = \"restore\"\ngone = \"none\"\n",
        )
        .unwrap_err();
        assert!(extra.contains("gone"), "{extra}");
        let unknown =
            RiskPolicy::parse("[tiers]\na = \"act\"\n[changes]\na = \"undo\"\n").unwrap_err();
        assert!(unknown.contains("unknown class"), "{unknown}");
    }
}
