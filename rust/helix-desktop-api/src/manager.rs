//! Agent1 *manager* role only: what the Settings app uses to see who may do what, change it, and read the audit. The manager is
//! recognised by the service from the canonical path of its executable. This module must never grow client verbs (`Hello`, `Action`,
//! `RequestAccess`) or the confirmer's (`RegisterConfirmer`, `Confirm`): the app that edits permissions is not an agent and does not approve.

use futures_util::StreamExt;
use zbus::zvariant::OwnedValue;
use zbus::Connection;

use crate::json::owned_to_json;
use crate::{BUS_NAME, OBJECT_PATH};

const INTERFACE: &str = "org.helix.Desktop.Agent1";

pub const MANAGER_METHODS: &[&str] = &[
    "RegisterManager",
    "GetGrants",
    "SetGrant",
    "RevokeClient",
    "GetAudit",
];

pub struct ManagerClient {
    connection: Connection,
}

impl ManagerClient {
    pub fn new(connection: Connection) -> Self {
        Self { connection }
    }

    async fn proxy(&self) -> zbus::Result<zbus::Proxy<'static>> {
        zbus::Proxy::new(&self.connection, BUS_NAME, OBJECT_PATH, INTERFACE).await
    }

    /// Claims the manager role. Refused by the service unless this executable is the installed Settings app.
    pub async fn register(&self) -> zbus::Result<()> {
        self.proxy().await?.call("RegisterManager", &()).await
    }

    fn array(rows: &[OwnedValue]) -> serde_json::Value {
        serde_json::Value::Array(rows.iter().map(owned_to_json).collect())
    }

    /// `[{client, tiers}]`.
    pub async fn grants(&self) -> zbus::Result<serde_json::Value> {
        let rows: Vec<OwnedValue> = self.proxy().await?.call("GetGrants", &()).await?;
        Ok(Self::array(&rows))
    }

    pub async fn set_grant(&self, client: &str, tiers: &[String]) -> zbus::Result<()> {
        self.proxy().await?.call("SetGrant", &(client, tiers)).await
    }

    pub async fn revoke(&self, client: &str) -> zbus::Result<()> {
        self.proxy().await?.call("RevokeClient", &(client,)).await
    }

    /// Audit rows at or after `since` (Unix seconds), at most `limit`, oldest first.
    pub async fn audit(&self, since: u64, limit: u32) -> zbus::Result<serde_json::Value> {
        let rows: Vec<OwnedValue> = self
            .proxy()
            .await?
            .call("GetAudit", &(since, limit))
            .await?;
        Ok(Self::array(&rows))
    }

    /// Calls `on_change` whenever grants change (any client, including this one).
    pub async fn watch<F>(&self, mut on_change: F) -> zbus::Result<()>
    where
        F: FnMut() + Send,
    {
        let mut stream = self.proxy().await?.receive_signal("Changed").await?;
        while stream.next().await.is_some() {
            on_change();
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::MANAGER_METHODS;
    use crate::{parse_interfaces, protocol_xml};

    #[test]
    fn the_manager_calls_are_in_the_contract() {
        let interfaces = parse_interfaces(protocol_xml());
        let agent = &interfaces["org.helix.Desktop.Agent1"];
        for method in MANAGER_METHODS {
            assert!(agent.methods.iter().any(|m| m == method), "{method}");
        }
    }

    #[test]
    fn the_manager_has_no_client_or_confirmer_verbs() {
        for forbidden in [
            "Hello",
            "RequestAccess",
            "Action",
            "Cancel",
            "ListTools",
            "GetSnapshot",
            "RegisterConfirmer",
            "Confirm",
        ] {
            assert!(!MANAGER_METHODS.contains(&forbidden), "{forbidden}");
        }
    }
}
