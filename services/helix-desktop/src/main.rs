//! Rust desktop authority for `org.helix.Desktop`.
//! Live sessions still use C++ `helix-service` until cutover — see `docs/design/cpp-to-rust.md`.
//! Nested harness: `HELIX_SERVICE_BIN=$PWD/target/debug/helix-desktop`.

mod agent;
mod agent_activity;
mod appearance;
mod audio;
mod audio_backend;
mod bluetooth;
mod bluetooth_backend;
mod configuration;
mod dbus_value;
mod displays;
mod network;
mod network_backend;
mod palette_catalog;
mod palette_previews;
mod peer;
mod runtime_state;
mod state;

use std::sync::Arc;

use tokio::sync::Mutex;
use zbus::connection;

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

    let state = Arc::new(Mutex::new(State::new()));
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
    let appearance = Appearance { state };

    let _connection = connection::Builder::session()?
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

    tracing::info!(
        bus = BUS_NAME,
        path = OBJECT_PATH,
        "helix-desktop listening"
    );

    tokio::signal::ctrl_c().await.ok();
    Ok(())
}
