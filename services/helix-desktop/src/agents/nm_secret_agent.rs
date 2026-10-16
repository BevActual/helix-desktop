//! NetworkManager SecretAgent on the system bus.

use std::collections::HashMap;
use std::sync::Arc;

use zbus::fdo;
use zbus::interface;
use zbus::zvariant::OwnedValue;

use super::hub::{AgentHub, NmSecretOutcome};

pub const OBJECT_PATH: &str = "/org/freedesktop/NetworkManager/SecretAgent";
const AGENT_IDENTIFIER: &str = "org.helix.SecretAgent";

const NM_BUS: &str = "org.freedesktop.NetworkManager";
const AGENT_MANAGER_PATH: &str = "/org/freedesktop/NetworkManager/AgentManager";
const AGENT_MANAGER_INTERFACE: &str = "org.freedesktop.NetworkManager.AgentManager";

#[derive(Debug, zbus::DBusError)]
#[zbus(prefix = "org.freedesktop.NetworkManager.SecretManager")]
enum NmSecretError {
    #[zbus(name = "NoSecrets")]
    NoSecrets,
    #[zbus(name = "UserCanceled")]
    UserCanceled,
}

pub struct NmSecretAgent {
    pub hub: Arc<AgentHub>,
}

#[interface(name = "org.freedesktop.NetworkManager.SecretAgent")]
impl NmSecretAgent {
    async fn get_secrets(
        &self,
        connection: HashMap<String, HashMap<String, OwnedValue>>,
        _connection_path: zbus::zvariant::ObjectPath<'_>,
        setting_name: String,
        _hints: Vec<String>,
        flags: u32,
    ) -> Result<HashMap<String, HashMap<String, OwnedValue>>, NmSecretError> {
        if !AgentHub::nm_flags_allow_interaction(flags) {
            return Err(NmSecretError::NoSecrets);
        }
        if !AgentHub::nm_setting_supported(&setting_name) {
            return Err(NmSecretError::NoSecrets);
        }
        let ssid = AgentHub::extract_ssid(&connection);
        tracing::info!(ssid, setting = setting_name, "GetSecrets prompt");
        match self.hub.begin_nm_secret(&ssid, setting_name.clone()).await {
            Ok(secret) => Ok(AgentHub::build_nm_secrets(&setting_name, &secret)),
            Err(NmSecretOutcome::NoSecrets(reason)) => {
                tracing::debug!(reason, "GetSecrets -> NoSecrets");
                Err(NmSecretError::NoSecrets)
            }
            Err(NmSecretOutcome::UserCanceled(reason)) => {
                tracing::debug!(reason, "GetSecrets -> UserCanceled");
                Err(NmSecretError::UserCanceled)
            }
        }
    }

    async fn cancel_get_secrets(
        &self,
        _connection_path: zbus::zvariant::ObjectPath<'_>,
        _setting_name: String,
    ) -> fdo::Result<()> {
        self.hub.cancel_nm_secret("NM canceled");
        Ok(())
    }

    async fn save_secrets(
        &self,
        _connection: HashMap<String, HashMap<String, OwnedValue>>,
        _connection_path: zbus::zvariant::ObjectPath<'_>,
    ) -> fdo::Result<()> {
        Ok(())
    }

    async fn delete_secrets(
        &self,
        _connection: HashMap<String, HashMap<String, OwnedValue>>,
        _connection_path: zbus::zvariant::ObjectPath<'_>,
    ) -> fdo::Result<()> {
        Ok(())
    }
}

pub async fn register(connection: &zbus::Connection) -> zbus::Result<()> {
    let proxy = zbus::Proxy::new(
        connection,
        NM_BUS,
        AGENT_MANAGER_PATH,
        AGENT_MANAGER_INTERFACE,
    )
    .await?;
    proxy
        .call::<_, _, ()>("Register", &(AGENT_IDENTIFIER,))
        .await?;
    tracing::info!(AGENT_IDENTIFIER, "registered NetworkManager secret agent");
    Ok(())
}

pub async fn unregister(connection: &zbus::Connection) {
    let proxy = zbus::Proxy::new(
        connection,
        NM_BUS,
        AGENT_MANAGER_PATH,
        AGENT_MANAGER_INTERFACE,
    )
    .await;
    if let Ok(proxy) = proxy {
        let _ = proxy.call::<_, _, ()>("Unregister", &()).await;
    }
}
