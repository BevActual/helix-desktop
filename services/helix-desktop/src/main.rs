//! Rust desktop authority for `org.helix.Desktop`.
//! Live sessions still use C++ `helix-service` until cutover — see `docs/design/cpp-to-rust.md`.
//! Nested harness: `HELIX_SERVICE_BIN=$PWD/target/debug/helix-desktop`.

mod agents;
mod desktop_operations;
mod agent;
mod agent_activity;
mod agent_support;
mod appearance;
mod audio;
mod audio_backend;
mod audio_meter;
mod audio_support;
mod bluetooth;
mod bluetooth_backend;
mod bluetooth_support;
mod configuration;
mod dbus_value;
mod displays;
mod displays_backend;
mod displays_support;
mod network;
mod network_backend;
mod network_support;
mod palette_catalog;
mod palette_previews;
mod peer;
mod runtime_state;
mod state;

use std::sync::Arc;

use tokio::sync::Mutex;
use zbus::connection;
use zbus::message::Type as MessageType;
use zbus::MessageStream;
use zbus::MatchRule;

use crate::agent::Agent;
use crate::agent_activity::AgentActivity;
use crate::appearance::Appearance;
use crate::audio::Audio;
use crate::bluetooth::Bluetooth;
use crate::configuration::Configuration;
use crate::displays::Displays;
use crate::network::Network;
use crate::state::State;

const BUS_NAME: &str = "org.helix.Desktop";
const OBJECT_PATH: &str = "/org/helix/Desktop";

#[tokio::main]
async fn main() -> zbus::Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("info")),
        )
        .init();

    let agents = agents::new_hub();
    let state = Arc::new(Mutex::new(State::new(agents)));
    let configuration = Configuration {
        state: Arc::clone(&state),
    };
    let agent = Agent {
        state: Arc::clone(&state),
    };
    let agent_activity = AgentActivity {
        state: Arc::clone(&state),
    };
    let audio = Audio {
        state: Arc::clone(&state),
    };
    let network = Network {
        state: Arc::clone(&state),
    };
    let bluetooth = Bluetooth {
        state: Arc::clone(&state),
    };
    let displays = Displays {
        state: Arc::clone(&state),
    };
    let appearance = Appearance {
        state: Arc::clone(&state),
    };

    let connection = connection::Builder::session()?
        .name(BUS_NAME)?
        .serve_at(OBJECT_PATH, configuration)?
        .serve_at(OBJECT_PATH, agent)?
        .serve_at(OBJECT_PATH, agent_activity)?
        .serve_at(OBJECT_PATH, audio)?
        .serve_at(OBJECT_PATH, network)?
        .serve_at(OBJECT_PATH, bluetooth)?
        .serve_at(OBJECT_PATH, displays)?
        .serve_at(OBJECT_PATH, appearance)?
        .build()
        .await?;

    {
        let mut locked = state.lock().await;
        locked.bus = Some(connection.clone());
    }
    let (use_nm, use_bluez) = {
        let hub = {
            let locked = state.lock().await;
            Arc::clone(&locked.agents)
        };
        hub.set_session(connection.clone()).await;
        let backends = {
            let locked = state.lock().await;
            (
                locked.network.uses_network_manager(),
                locked.bluetooth.uses_bluez(),
            )
        };
        agents::start_system_agents(hub, backends.0, backends.1);
        backends
    };

    let rule = MatchRule::builder()
        .msg_type(MessageType::Signal)
        .interface("org.freedesktop.DBus")?
        .member("NameOwnerChanged")?
        .build();
    let mut stream = MessageStream::for_match_rule(rule, &connection, None).await?;
    let watch_state = Arc::clone(&state);
    tokio::spawn(async move {
        use futures_util::StreamExt;
        while let Some(message) = stream.next().await {
            let Ok(message) = message else {
                continue;
            };
            let Ok(body) = message.body().deserialize::<(String, String, String)>() else {
                continue;
            };
            let (_name, old_owner, _new_owner) = body;
            if old_owner.is_empty() {
                continue;
            }
            let mut locked = watch_state.lock().await;
            if locked.confirmer.as_deref() == Some(old_owner.as_str()) {
                agent_support::confirmer_disconnected(&mut locked).await;
            }
            locked.agents.on_peer_disconnected(&old_owner);
            locked.audio_meters.remove_owner(&old_owner);
            desktop_operations::clear_operations_for_peer(&mut locked, &old_owner);
        }
    });

    let expire_state = Arc::clone(&state);
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(std::time::Duration::from_secs(5));
        interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        loop {
            interval.tick().await;
            let mut locked = expire_state.lock().await;
            agent_support::expire_confirmations(&mut locked).await;
        }
    });

    let displays_poll_state = Arc::clone(&state);
    let displays_conn = connection.clone();
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(std::time::Duration::from_secs(3));
        interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        loop {
            interval.tick().await;
            let poll_state = Arc::clone(&displays_poll_state);
            let changed = tokio::task::spawn_blocking(move || {
                let mut locked = poll_state.blocking_lock();
                let desktop = locked.desktop.is_some();
                locked.displays.refresh(desktop)
            })
            .await
            .unwrap_or(false);
            if changed {
                displays_support::emit_displays_changed(&displays_conn).await;
            }
        }
    });

    let bluetooth_poll_state = Arc::clone(&state);
    let bluetooth_conn = connection.clone();
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(std::time::Duration::from_secs(3));
        interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        let mut last_available = {
            let locked = bluetooth_poll_state.lock().await;
            locked.bluetooth.is_available()
        };
        loop {
            interval.tick().await;
            let poll_state = Arc::clone(&bluetooth_poll_state);
            let (changed, available) = tokio::task::spawn_blocking(move || {
                let mut locked = poll_state.blocking_lock();
                let changed = locked.bluetooth.refresh();
                let available = locked.bluetooth.is_available();
                (changed, available)
            })
            .await
            .unwrap_or((false, false));
            let config_changed = available != last_available;
            last_available = available;
            if changed {
                bluetooth_support::emit_bluetooth_changed(&bluetooth_conn).await;
            }
            if config_changed {
                bluetooth_support::emit_configuration_changed(&bluetooth_conn).await;
            }
        }
    });

    let network_poll_state = Arc::clone(&state);
    let network_conn = connection.clone();
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(std::time::Duration::from_secs(3));
        interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        let mut last_available = {
            let locked = network_poll_state.lock().await;
            locked.network.is_available()
        };
        loop {
            interval.tick().await;
            let poll_state = Arc::clone(&network_poll_state);
            let (changed, available) = tokio::task::spawn_blocking(move || {
                let mut locked = poll_state.blocking_lock();
                let changed = locked.network.refresh();
                let available = locked.network.is_available();
                (changed, available)
            })
            .await
            .unwrap_or((false, false));
            let config_changed = available != last_available;
            last_available = available;
            if changed {
                network_support::emit_network_changed(&network_conn).await;
            }
            if config_changed {
                network_support::emit_configuration_changed(&network_conn).await;
            }
        }
    });

    let operations_state = Arc::clone(&state);
    let operations_conn = connection.clone();
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(std::time::Duration::from_secs(1));
        interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        loop {
            interval.tick().await;
            let mut locked = operations_state.lock().await;
            desktop_operations::poll_operations(&mut locked, &operations_conn).await;
        }
    });

    let meter_state = Arc::clone(&state);
    let meter_conn = connection.clone();
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(std::time::Duration::from_millis(80));
        interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        loop {
            interval.tick().await;
            let samples = {
                let locked = meter_state.lock().await;
                locked.audio_meters.poll()
            };
            for (owner, source, peak, available) in samples {
                audio_support::emit_input_level(&meter_conn, &owner, source, peak, available)
                    .await;
            }
        }
    });

    let audio_poll_state = Arc::clone(&state);
    let audio_conn = connection.clone();
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(std::time::Duration::from_secs(3));
        interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
        let mut last_available = {
            let locked = audio_poll_state.lock().await;
            locked.audio.is_available()
        };
        loop {
            interval.tick().await;
            let mut locked = audio_poll_state.lock().await;
            let changed = locked.audio.refresh();
            let available = locked.audio.is_available();
            let config_changed = available != last_available;
            last_available = available;
            drop(locked);
            if changed {
                audio_support::emit_audio_changed(&audio_conn).await;
            }
            if config_changed {
                audio_support::emit_configuration_changed(&audio_conn).await;
            }
        }
    });

    tracing::info!(
        bus = BUS_NAME,
        path = OBJECT_PATH,
        "helix-desktop listening"
    );

    tokio::signal::ctrl_c().await.ok();
    agents::shutdown_system_agents(use_nm, use_bluez).await;
    Ok(())
}
