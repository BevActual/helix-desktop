//! System-bus pairing / secret agents bridged to session-bus Prompt signals.

mod bluez_agent;
mod hub;
mod nm_secret_agent;

use std::sync::Arc;

pub use hub::AgentHub;

pub fn new_hub() -> Arc<AgentHub> {
    Arc::new(AgentHub::new())
}

pub fn start_system_agents(hub: Arc<AgentHub>, nm: bool, bluez: bool) {
    if !nm && !bluez {
        return;
    }
    tokio::spawn(async move {
        let system = match zbus::Connection::system().await {
            Ok(connection) => connection,
            Err(error) => {
                tracing::warn!(%error, "system bus connection failed; agents disabled");
                return;
            }
        };
        if nm {
            if let Err(error) = system
                .object_server()
                .at(nm_secret_agent::OBJECT_PATH, nm_secret_agent::NmSecretAgent {
                    hub: Arc::clone(&hub),
                })
                .await
            {
                tracing::warn!(%error, "NM secret agent export failed");
            } else if let Err(error) = nm_secret_agent::register(&system).await {
                tracing::warn!(%error, "NM secret agent registration failed");
            }
        }
        if bluez {
            if let Err(error) = system
                .object_server()
                .at(bluez_agent::OBJECT_PATH, bluez_agent::BluezAgent {
                    hub: Arc::clone(&hub),
                })
                .await
            {
                tracing::warn!(%error, "BlueZ agent export failed");
            } else if let Err(error) = bluez_agent::register(&system).await {
                tracing::warn!(%error, "BlueZ agent registration failed");
            }
        }
        std::future::pending::<()>().await;
    });
}

pub async fn shutdown_system_agents(nm: bool, bluez: bool) {
    if !nm && !bluez {
        return;
    }
    let system = match zbus::Connection::system().await {
        Ok(connection) => connection,
        Err(error) => {
            tracing::warn!(%error, "system bus unavailable during agent shutdown");
            return;
        }
    };
    if nm {
        nm_secret_agent::unregister(&system).await;
    }
    if bluez {
        bluez_agent::unregister(&system).await;
    }
}
