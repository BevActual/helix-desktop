//! System-bus pairing / secret agents bridged to session-bus Prompt signals.

mod bluez_agent;
mod hub;
mod nm_secret_agent;

use std::sync::Arc;

pub use hub::AgentHub;

pub fn new_hub() -> Arc<AgentHub> {
    Arc::new(AgentHub::new())
}

pub async fn start_system_agents(hub: Arc<AgentHub>, nm: bool, bluez: bool) {
    if !nm && !bluez {
        return;
    }
    let mut shutdown_rx = hub.system_shutdown_rx();
    let hub_task = Arc::clone(&hub);
    let handle = tokio::spawn(async move {
        let system = match zbus::Connection::system().await {
            Ok(connection) => connection,
            Err(error) => {
                tracing::warn!(%error, "system bus connection failed; agents disabled");
                return;
            }
        };
        // The daemons may start after this service; keep trying until each agent is registered
        // (or the service shuts down).
        let mut nm_pending = nm;
        let mut bluez_pending = bluez;
        let mut nm_exported = false;
        let mut bluez_exported = false;
        while (nm_pending || bluez_pending) && !*shutdown_rx.borrow() {
            if nm_pending {
                if !nm_exported {
                    match system
                        .object_server()
                        .at(
                            nm_secret_agent::OBJECT_PATH,
                            nm_secret_agent::NmSecretAgent {
                                hub: Arc::clone(&hub_task),
                            },
                        )
                        .await
                    {
                        Ok(_) => nm_exported = true,
                        Err(error) => {
                            tracing::warn!(%error, "NM secret agent export failed");
                            nm_pending = false;
                        }
                    }
                }
                if nm_exported {
                    match nm_secret_agent::register(&system).await {
                        Ok(_) => nm_pending = false,
                        Err(error) => {
                            tracing::debug!(%error, "NM secret agent registration pending")
                        }
                    }
                }
            }
            if bluez_pending {
                if !bluez_exported {
                    match system
                        .object_server()
                        .at(
                            bluez_agent::OBJECT_PATH,
                            bluez_agent::BluezAgent {
                                hub: Arc::clone(&hub_task),
                            },
                        )
                        .await
                    {
                        Ok(_) => bluez_exported = true,
                        Err(error) => {
                            tracing::warn!(%error, "BlueZ agent export failed");
                            bluez_pending = false;
                        }
                    }
                }
                if bluez_exported {
                    match bluez_agent::register(&system).await {
                        Ok(_) => bluez_pending = false,
                        Err(error) => tracing::debug!(%error, "BlueZ agent registration pending"),
                    }
                }
            }
            if nm_pending || bluez_pending {
                tokio::select! {
                    _ = tokio::time::sleep(std::time::Duration::from_secs(3)) => {}
                    _ = shutdown_rx.changed() => {}
                }
            }
        }

        while !*shutdown_rx.borrow_and_update() {
            if shutdown_rx.changed().await.is_err() {
                break;
            }
        }

        if nm {
            nm_secret_agent::unregister(&system).await;
            let _ = system
                .object_server()
                .remove::<nm_secret_agent::NmSecretAgent, _>(nm_secret_agent::OBJECT_PATH)
                .await;
        }
        if bluez {
            bluez_agent::unregister(&system).await;
            let _ = system
                .object_server()
                .remove::<bluez_agent::BluezAgent, _>(bluez_agent::OBJECT_PATH)
                .await;
        }
        tracing::info!("system-bus agents unregistered");
    });
    hub.set_system_task(handle).await;
}

pub async fn shutdown_system_agents(hub: &AgentHub, nm: bool, bluez: bool) {
    if !nm && !bluez {
        return;
    }
    hub.signal_system_shutdown();
    hub.join_system_task().await;
}
