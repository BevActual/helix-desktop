//! Agent Surface confirmer role only. The shell registers as confirmer, receives confirmation requests,
//! and answers them. It must never call Hello, RequestAccess, Action, or the manager methods.

use std::collections::HashMap;

use futures_util::StreamExt;
use zbus::zvariant::{OwnedValue, Value};
use zbus::Connection;

use crate::json::owned_to_json;
use crate::{BUS_NAME, OBJECT_PATH};

const INTERFACE: &str = "org.helix.Desktop.AgentSurface1";

/// Methods and signals this crate is allowed to use on Agent Surface. Everything else is forbidden in
/// the shell boundary test.
pub const CONFIRMER_METHODS: &[&str] = &["RegisterConfirmer", "Confirm"];
pub const CONFIRMER_SIGNALS: &[&str] = &["ConfirmationRequested", "ConfirmationEnded"];

pub struct ConfirmerClient {
    connection: Connection,
}

impl ConfirmerClient {
    pub fn new(connection: Connection) -> Self {
        Self { connection }
    }

    async fn proxy(&self) -> zbus::Result<zbus::Proxy<'static>> {
        zbus::Proxy::new(&self.connection, BUS_NAME, OBJECT_PATH, INTERFACE).await
    }

    /// The desktop must already own the session (`RegisterDesktop`) before this succeeds.
    pub async fn register(&self) -> zbus::Result<()> {
        let proxy = self.proxy().await?;
        proxy.call("RegisterConfirmer", &()).await
    }

    pub async fn confirm(
        &self,
        operation: u64,
        approved: bool,
        tiers: &[String],
    ) -> zbus::Result<()> {
        let proxy = self.proxy().await?;
        let mut options: HashMap<String, Value<'_>> = HashMap::new();
        if approved && !tiers.is_empty() {
            let list: Vec<Value<'_>> = tiers
                .iter()
                .map(|tier| Value::from(tier.as_str()))
                .collect();
            options.insert("tiers".into(), Value::Array(list.into()));
        }
        proxy.call("Confirm", &(operation, approved, options)).await
    }

    /// Calls `on_request` with the ConfirmationRequested payload as JSON for each signal.
    pub async fn watch_requested<F>(&self, mut on_request: F) -> zbus::Result<()>
    where
        F: FnMut(serde_json::Value) + Send,
    {
        let proxy = self.proxy().await?;
        let mut stream = proxy.receive_signal("ConfirmationRequested").await?;
        while let Some(message) = stream.next().await {
            let body: HashMap<String, OwnedValue> = message.body().deserialize()?;
            on_request(details_from_owned(&body));
        }
        Ok(())
    }

    /// Calls `on_ended` with the operation id for each ConfirmationEnded signal.
    pub async fn watch_ended<F>(&self, mut on_ended: F) -> zbus::Result<()>
    where
        F: FnMut(u64) + Send,
    {
        let proxy = self.proxy().await?;
        let mut stream = proxy.receive_signal("ConfirmationEnded").await?;
        while let Some(message) = stream.next().await {
            let operation: u64 = message.body().deserialize()?;
            on_ended(operation);
        }
        Ok(())
    }
}

pub fn details_from_owned(map: &HashMap<String, OwnedValue>) -> serde_json::Value {
    let mut object = serde_json::Map::new();
    for (key, value) in map {
        object.insert(key.clone(), owned_to_json(value));
    }
    serde_json::Value::Object(object)
}

#[cfg(test)]
mod tests {
    use super::{CONFIRMER_METHODS, CONFIRMER_SIGNALS};
    use crate::parse_interfaces;
    use crate::protocol_xml;

    #[test]
    fn confirmer_calls_are_listed_in_the_contract() {
        let interfaces = parse_interfaces(protocol_xml());
        let agent = &interfaces["org.helix.Desktop.AgentSurface1"];
        for method in CONFIRMER_METHODS {
            assert!(agent.methods.iter().any(|name| name == method), "{method}");
        }
        for signal in CONFIRMER_SIGNALS {
            assert!(agent.signals.iter().any(|name| name == signal), "{signal}");
        }
    }

    #[test]
    fn confirmer_surface_does_not_include_client_or_manager_verbs() {
        for forbidden in [
            "Hello",
            "RequestAccess",
            "Action",
            "GetSnapshot",
            "ListTools",
            "RegisterManager",
            "GetGrants",
            "SetGrant",
            "RevokeClient",
            "GetAudit",
        ] {
            assert!(!CONFIRMER_METHODS.contains(&forbidden), "{forbidden}");
        }
    }
}
