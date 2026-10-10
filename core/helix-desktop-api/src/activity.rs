//! `org.helix.Desktop.AgentSurfaceActivity1`: what agents did and are waiting to do, stopping them, revoking
//! access, and the kill switch. Only the registered desktop may call it; the shell must never use
//! `SetGrant` (that is the Settings manager role) or any client verb of Agent Surface.

use futures_util::StreamExt;
use zbus::zvariant::OwnedValue;
use zbus::Connection;

use crate::json::owned_to_json;
use crate::{BUS_NAME, OBJECT_PATH};

const INTERFACE: &str = "org.helix.Desktop.AgentSurfaceActivity1";

pub const ACTIVITY_METHODS: &[&str] = &[
    "GetActivity",
    "ListPending",
    "Stop",
    "StopAll",
    "GetGrants",
    "RevokeClient",
    "GetEnabled",
    "SetEnabled",
];
pub const ACTIVITY_SIGNALS: &[&str] = &["Changed", "PendingChanged"];

pub struct ActivityClient {
    connection: Connection,
}

impl ActivityClient {
    pub fn new(connection: Connection) -> Self {
        Self { connection }
    }

    async fn proxy(&self) -> zbus::Result<zbus::Proxy<'static>> {
        zbus::Proxy::new(&self.connection, BUS_NAME, OBJECT_PATH, INTERFACE).await
    }

    fn to_array(rows: &[OwnedValue]) -> serde_json::Value {
        serde_json::Value::Array(rows.iter().map(owned_to_json).collect())
    }

    /// Audit rows at or after `since` (Unix seconds), at most `limit`, oldest first.
    pub async fn activity(&self, since: u64, limit: u32) -> zbus::Result<serde_json::Value> {
        let rows: Vec<OwnedValue> = self
            .proxy()
            .await?
            .call("GetActivity", &(since, limit))
            .await?;
        Ok(Self::to_array(&rows))
    }

    pub async fn pending(&self) -> zbus::Result<serde_json::Value> {
        let rows: Vec<OwnedValue> = self.proxy().await?.call("ListPending", &()).await?;
        Ok(Self::to_array(&rows))
    }

    pub async fn grants(&self) -> zbus::Result<serde_json::Value> {
        let rows: Vec<OwnedValue> = self.proxy().await?.call("GetGrants", &()).await?;
        Ok(Self::to_array(&rows))
    }

    pub async fn enabled(&self) -> zbus::Result<bool> {
        self.proxy().await?.call("GetEnabled", &()).await
    }

    pub async fn stop(&self, operation: u64) -> zbus::Result<()> {
        self.proxy().await?.call("Stop", &(operation,)).await
    }

    pub async fn stop_all(&self) -> zbus::Result<()> {
        self.proxy().await?.call("StopAll", &()).await
    }

    pub async fn revoke(&self, client: &str) -> zbus::Result<()> {
        self.proxy().await?.call("RevokeClient", &(client,)).await
    }

    pub async fn set_enabled(&self, enabled: bool) -> zbus::Result<()> {
        self.proxy().await?.call("SetEnabled", &(enabled,)).await
    }

    /// Calls `on_change` for every `Changed` or `PendingChanged` signal.
    pub async fn watch<F>(&self, mut on_change: F) -> zbus::Result<()>
    where
        F: FnMut() + Send,
    {
        let proxy = self.proxy().await?;
        let changed = proxy.receive_signal("Changed").await?.map(|_| ());
        let pending = proxy.receive_signal("PendingChanged").await?.map(|_| ());
        let mut both = futures_util::stream::select(changed, pending);
        while both.next().await.is_some() {
            on_change();
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::{ACTIVITY_METHODS, ACTIVITY_SIGNALS};
    use crate::{parse_interfaces, protocol_xml};

    #[test]
    fn activity_calls_match_the_contract_exactly() {
        let interfaces = parse_interfaces(protocol_xml());
        let activity = &interfaces["org.helix.Desktop.AgentSurfaceActivity1"];
        let mut methods: Vec<&str> = activity.methods.iter().map(String::as_str).collect();
        methods.sort_unstable();
        let mut used = ACTIVITY_METHODS.to_vec();
        used.sort_unstable();
        assert_eq!(methods, used);
        for signal in ACTIVITY_SIGNALS {
            assert!(
                activity.signals.iter().any(|name| name == signal),
                "{signal}"
            );
        }
    }

    #[test]
    fn the_shell_cannot_widen_grants() {
        for forbidden in [
            "SetGrant",
            "RegisterManager",
            "Action",
            "RequestAccess",
            "Hello",
        ] {
            assert!(!ACTIVITY_METHODS.contains(&forbidden), "{forbidden}");
        }
    }
}
