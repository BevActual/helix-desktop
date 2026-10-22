//! A client of the desktop's Agent Surface (`org.helix.Desktop.AgentSurface1`) for Mobius's own tool loop. Mobius is an agent client like any other:
//! it asks for a tier, the person grants it (and confirms the risky commands) in the shell, and every command is audited there. This module only
//! speaks the protocol; it never decides what is allowed.

use futures_util::StreamExt;
use std::collections::{HashMap, HashSet};
use std::time::Duration;
use zbus::zvariant::{OwnedValue, Value};

const BUS: &str = "org.helix.Desktop";
const PATH: &str = "/org/helix/Desktop";
const IFACE: &str = "org.helix.Desktop.AgentSurface1";

pub struct Surface {
    proxy: zbus::Proxy<'static>,
    completed: zbus::proxy::SignalStream<'static>,
    tiers: HashSet<String>,
    _connection: zbus::Connection,
}

/// What a command answered. (The planner is always told tool output is untrusted data.)
pub struct Reply {
    pub text: String,
}

/// The text of a `display-screenshot` reply's picture path (`ok /path.png`).
pub fn picture_path(reply: &str) -> Option<&str> {
    reply
        .lines()
        .rev()
        .find_map(|line| line.strip_prefix("ok /").map(|_| line[3..].trim()))
}

impl Surface {
    /// Connects on a bus connection of its own, so the desktop sees Mobius as one client, and says hello.
    pub async fn connect(label: &str) -> Result<Self, String> {
        let connection = zbus::Connection::session()
            .await
            .map_err(|e| format!("No desktop session: {e}"))?;
        let proxy = zbus::Proxy::new(&connection, BUS, PATH, IFACE)
            .await
            .map_err(|e| e.to_string())?;
        let completed = proxy
            .receive_signal("Completed")
            .await
            .map_err(|e| e.to_string())?;
        let (session,): (HashMap<String, OwnedValue>,) =
            proxy
                .call("Hello", &(label, "1"))
                .await
                .map_err(|e| format!("The desktop does not offer agent access: {e}"))?;
        let tiers = session
            .get("tiers")
            .and_then(|v| <Vec<String>>::try_from(v.try_clone().ok()?).ok())
            .unwrap_or_default()
            .into_iter()
            .collect();
        Ok(Self {
            proxy,
            completed,
            tiers,
            _connection: connection,
        })
    }

    async fn wait(&mut self, operation: u64, seconds: u64) -> Result<(bool, String), String> {
        let deadline = tokio::time::sleep(Duration::from_secs(seconds));
        tokio::pin!(deadline);
        loop {
            tokio::select! {
                _ = &mut deadline => return Err("The desktop did not answer in time (is a confirmation waiting?).".into()),
                signal = self.completed.next() => {
                    let signal = signal.ok_or("The desktop connection ended.")?;
                    let (own, ok, body): (u64, bool, String) = signal.body().deserialize().map_err(|e| e.to_string())?;
                    if own == operation { return Ok((ok, body)); }
                }
            }
        }
    }

    /// Asks the person (once per tier) for a tier. A refusal is final.
    pub async fn ensure(&mut self, tier: &str) -> Result<(), String> {
        if self.tiers.contains(tier) {
            return Ok(());
        }
        let (operation,): (u64,) = self
            .proxy
            .call("RequestAccess", &(vec![tier.to_string()],))
            .await
            .map_err(|e| e.to_string())?;
        let (ok, body) = self.wait(operation, 90).await?;
        let value: serde_json::Value = serde_json::from_str(&body).unwrap_or_default();
        if !ok {
            return Err(value
                .get("error")
                .and_then(|v| v.as_str())
                .unwrap_or("Access was not granted.")
                .into());
        }
        let granted = value
            .get("granted")
            .and_then(|v| v.as_array())
            .into_iter()
            .flatten()
            .filter_map(|v| v.as_str().map(str::to_string));
        self.tiers.extend(granted);
        if self.tiers.contains(tier) {
            Ok(())
        } else {
            Err(format!("The {tier} tier was not granted."))
        }
    }

    /// Runs one `helix msg` command. `literal` is model-written text, passed after `--` so it is never read as a flag.
    pub async fn run(
        &mut self,
        tool: &str,
        argv: &[String],
        literal: &[String],
        seconds: u64,
    ) -> Result<Reply, String> {
        let mut options: HashMap<&str, Value<'_>> = HashMap::new();
        options.insert("argv", Value::from(argv.to_vec()));
        options.insert("literal", Value::from(literal.to_vec()));
        options.insert("reason", Value::from("Mobius agent task".to_string()));
        let (operation,): (u64,) = self
            .proxy
            .call("Action", &(tool, options))
            .await
            .map_err(|e| e.to_string())?;
        let (ok, body) = self.wait(operation, seconds.max(75)).await?;
        let value: serde_json::Value = serde_json::from_str(&body)
            .unwrap_or_else(|_| serde_json::json!({"trust":"untrusted","content":body}));
        let field = |key: &str| value.get(key).and_then(|v| v.as_str()).map(str::to_string);
        if !ok {
            return Err(field("error").unwrap_or_else(|| "The command failed.".into()));
        }
        Ok(Reply {
            text: field("content").unwrap_or_default(),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_picture_path_is_the_last_ok_line() {
        assert_eq!(
            picture_path("size 1280x720\nok /run/user/1000/shots/a.png"),
            Some("/run/user/1000/shots/a.png")
        );
        assert_eq!(picture_path("error: the display is not running"), None);
        assert_eq!(picture_path("ok done"), None);
    }
}
