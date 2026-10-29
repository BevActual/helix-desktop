//! Persisted Agent Surface grants (`grants.json`). Port of C++ `helix::agent::GrantStore`.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use serde_json::{json, Value};

use super::risk::Tier;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct GrantStore {
    file: PathBuf,
    grants: BTreeMap<String, BTreeSet<Tier>>,
}

impl GrantStore {
    pub fn new(file: impl Into<PathBuf>) -> Self {
        Self {
            file: file.into(),
            grants: BTreeMap::new(),
        }
    }

    pub fn path(&self) -> &Path {
        &self.file
    }

    /// Missing file → empty store. Corrupt file → error and empty grants.
    pub fn load(&mut self) -> Result<(), String> {
        self.grants.clear();
        let Ok(text) = fs::read_to_string(&self.file) else {
            return Ok(());
        };
        let value: Value =
            serde_json::from_str(&text).map_err(|error| format!("grants: {error}"))?;
        let Some(clients) = value.get("clients").and_then(Value::as_object) else {
            return Err("grants: missing clients object".into());
        };
        for (client, tiers) in clients {
            let mut parsed = BTreeSet::new();
            if let Some(list) = tiers.as_array() {
                for name in list {
                    if let Some(tier) = name.as_str().and_then(Tier::parse) {
                        if tier != Tier::Blocked {
                            parsed.insert(tier);
                        }
                    }
                }
            }
            self.grants.insert(client.clone(), parsed);
        }
        Ok(())
    }

    pub fn save(&self) -> Result<(), String> {
        let mut clients = serde_json::Map::new();
        for (client, tiers) in &self.grants {
            let names: Vec<Value> = tiers
                .iter()
                .map(|tier| Value::String(tier.name().into()))
                .collect();
            clients.insert(client.clone(), Value::Array(names));
        }
        let body = format!(
            "{}\n",
            serde_json::to_string_pretty(&json!({ "version": 1, "clients": clients }))
                .map_err(|error| format!("grants: {error}"))?
        );
        if let Some(parent) = self.file.parent() {
            fs::create_dir_all(parent).map_err(|error| format!("grants: {error}"))?;
        }
        let temporary = self.file.with_extension("json.tmp");
        {
            let mut options = fs::OpenOptions::new();
            options.write(true).create(true).truncate(true);
            #[cfg(unix)]
            {
                use std::os::unix::fs::OpenOptionsExt;
                options.mode(0o600);
            }
            let mut file = options
                .open(&temporary)
                .map_err(|error| format!("grants: {error}"))?;
            file.write_all(body.as_bytes())
                .map_err(|error| format!("grants: {error}"))?;
        }
        fs::rename(&temporary, &self.file).map_err(|error| {
            let _ = fs::remove_file(&temporary);
            format!("grants: {error}")
        })?;
        Ok(())
    }

    pub fn set(&mut self, client: &str, tiers: impl IntoIterator<Item = Tier>) {
        let mut set: BTreeSet<Tier> = tiers
            .into_iter()
            .filter(|tier| *tier != Tier::Blocked)
            .collect();
        if set.is_empty() {
            self.grants.remove(client);
        } else {
            self.grants
                .insert(client.to_string(), std::mem::take(&mut set));
        }
    }

    pub fn revoke(&mut self, client: &str) {
        self.grants.remove(client);
    }

    pub fn allows(&self, client: &str, tier: Tier) -> bool {
        if tier == Tier::Blocked {
            return false;
        }
        self.grants
            .get(client)
            .is_some_and(|tiers| tiers.contains(&tier))
    }

    pub fn tiers_for(&self, client: &str) -> BTreeSet<Tier> {
        self.grants.get(client).cloned().unwrap_or_default()
    }

    pub fn clients(&self) -> Vec<String> {
        self.grants.keys().cloned().collect()
    }

    /// Rows shaped like `[{ "client", "tiers": [...] }]` for GetGrants.
    pub fn as_rows(&self) -> Vec<Value> {
        self.grants
            .iter()
            .map(|(client, tiers)| {
                json!({
                    "client": client,
                    "tiers": tiers.iter().map(|tier| tier.name()).collect::<Vec<_>>(),
                })
            })
            .collect()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Verdict {
    Allow,
    NeedsConfirmation,
    Deny,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Decision {
    pub verdict: Verdict,
    pub tier: Tier,
    pub tainted: bool,
    pub reason: String,
}

#[derive(Clone, Debug, Default)]
pub struct TaintTracker {
    tainted: BTreeSet<String>,
}

impl TaintTracker {
    pub fn mark_untrusted(&mut self, client: &str) {
        self.tainted.insert(client.to_string());
    }

    pub fn clear(&mut self, client: &str) {
        self.tainted.remove(client);
    }

    pub fn tainted(&self, client: &str) -> bool {
        self.tainted.contains(client)
    }
}

pub fn result_is_untrusted(tier: Tier) -> bool {
    tier == Tier::ObservePrivate
}

pub fn evaluate(
    policy: &super::risk::RiskPolicy,
    grants: &GrantStore,
    taint: &TaintTracker,
    client: &str,
    command: &str,
    args: &[String],
) -> Decision {
    evaluate_tier(
        policy.tier_for(command, args),
        policy.knows(command),
        grants,
        taint,
        client,
    )
}

/// The same decision for a tier that comes from somewhere other than `msg-risk.toml` (the actions catalogue, `helix-actions`):
/// blocked is refused, a missing grant is refused, confirm asks.
pub fn evaluate_tier(
    tier: Tier,
    known: bool,
    grants: &GrantStore,
    taint: &TaintTracker,
    client: &str,
) -> Decision {
    let tainted = taint.tainted(client);
    if tier == Tier::Blocked {
        return Decision {
            verdict: Verdict::Deny,
            tier,
            tainted,
            reason: if known {
                "blocked for agent clients".into()
            } else {
                "unknown command".into()
            },
        };
    }
    if !grants.allows(client, tier) {
        return Decision {
            verdict: Verdict::Deny,
            tier,
            tainted,
            reason: format!("client has no {} grant", tier.name()),
        };
    }
    Decision {
        verdict: if tier == Tier::Confirm {
            Verdict::NeedsConfirmation
        } else {
            Verdict::Allow
        },
        tier,
        tainted,
        reason: String::new(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::risk::RiskPolicy;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_grants() -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        std::env::temp_dir().join(format!("helix-grants-{nanos}.json"))
    }

    #[test]
    fn round_trip_and_evaluate() {
        let path = temp_grants();
        let mut store = GrantStore::new(&path);
        store.set("exe:/bin/agent", [Tier::Observe, Tier::Act]);
        store.save().unwrap();
        let mut loaded = GrantStore::new(&path);
        loaded.load().unwrap();
        assert!(loaded.allows("exe:/bin/agent", Tier::Act));
        assert!(!loaded.allows("exe:/bin/agent", Tier::Confirm));

        let policy = RiskPolicy::embedded();
        let taint = TaintTracker::default();
        let allowed = evaluate(&policy, &loaded, &taint, "exe:/bin/agent", "status", &[]);
        assert_eq!(allowed.verdict, Verdict::Allow);
        let denied = evaluate(
            &policy,
            &loaded,
            &taint,
            "exe:/bin/agent",
            "window-close",
            &[],
        );
        assert_eq!(denied.verdict, Verdict::Deny);
        let _ = fs::remove_file(path);
    }
}
