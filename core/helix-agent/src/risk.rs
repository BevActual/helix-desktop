//! Risk tiers from `protocols/ipc/msg-risk.toml`. Unknown commands are Blocked (fail closed).
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

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RiskPolicy {
    tiers: BTreeMap<String, Tier>,
    /// Keys are `"command first-arg"` for choice overrides.
    choices: BTreeMap<String, Tier>,
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn from_env_reads_policy_file() {
        let dir = std::env::temp_dir().join(format!("helix-risk-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let file = dir.join("custom.toml");
        std::fs::write(
            &file,
            "[tiers]\nonly-cmd = \"observe\"\n",
        )
        .unwrap();
        std::env::set_var("HELIX_RISK_POLICY", &file);
        let policy = RiskPolicy::from_env();
        assert!(policy.knows("only-cmd"));
        assert!(!policy.knows("status"));
        std::env::remove_var("HELIX_RISK_POLICY");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn embedded_policy_loads_and_fail_closed() {
        let policy = RiskPolicy::embedded();
        assert!(policy.knows("status"));
        assert_eq!(policy.tier_for("status", &[]), Tier::Observe);
        assert_eq!(policy.tier_for("not-a-real-command", &[]), Tier::Blocked);
    }

    #[test]
    fn choice_override() {
        let policy = RiskPolicy::embedded();
        assert_eq!(policy.tier_for("session", &[]), Tier::Blocked);
        let lock = policy.tier_for("session", &["lock".into()]);
        assert_ne!(lock, Tier::Blocked, "choices should override session lock");
    }
}
