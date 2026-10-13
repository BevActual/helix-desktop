use std::collections::HashMap;
use std::sync::Arc;

use tokio::sync::Mutex;
use zbus::fdo;
use zbus::message::Header;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::OwnedValue;
use zbus::interface;
use zbus::Connection;

use crate::agent_support::{cancel_all_pending, finish_pending_denied};
use crate::dbus_value::{dict_entry, json_to_owned, owned_bool, owned_str, owned_u64};
use crate::state::State;

pub struct AgentActivity {
    pub state: Arc<Mutex<State>>,
}

fn require_desktop(state: &State, sender: &str) -> fdo::Result<()> {
    match &state.desktop {
        Some(owner) if owner == sender => Ok(()),
        _ => Err(fdo::Error::AccessDenied(
            "Only the registered shell may use agent activity".into(),
        )),
    }
}

impl AgentActivity {
    pub async fn notify_changed(connection: &Connection) {
        if let Ok(iface) = connection
            .object_server()
            .interface::<_, AgentActivity>(crate::agent_support::OBJECT_PATH)
            .await
        {
            Self::changed(iface.signal_emitter()).await.ok();
        }
    }

    pub async fn notify_pending_changed(connection: &Connection) {
        if let Ok(iface) = connection
            .object_server()
            .interface::<_, AgentActivity>(crate::agent_support::OBJECT_PATH)
            .await
        {
            Self::pending_changed(iface.signal_emitter()).await.ok();
        }
    }
}

#[interface(name = "org.helix.Desktop.AgentSurfaceActivity1")]
impl AgentActivity {
    async fn get_activity(
        &self,
        #[zbus(header)] header: Header<'_>,
        _since: u64,
        limit: u32,
    ) -> fdo::Result<Vec<OwnedValue>> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        Ok(state
            .audit
            .read_recent(_since, limit)
            .iter()
            .filter_map(json_to_owned)
            .collect())
    }

    async fn list_pending(
        &self,
        #[zbus(header)] header: Header<'_>,
    ) -> fdo::Result<Vec<OwnedValue>> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        Ok(state
            .pending
            .iter()
            .map(|(operation, pending)| {
                dict_entry(HashMap::from([
                    ("operation", owned_u64(*operation)),
                    ("client", owned_str(&pending.client)),
                    ("label", owned_str(&pending.label)),
                    ("tool", owned_str(&pending.tool)),
                    ("reason", owned_str(&pending.reason)),
                    ("tainted", owned_bool(pending.tainted)),
                ]))
            })
            .collect())
    }

    async fn stop(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        operation: u64,
    ) -> fdo::Result<()> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let mut state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        let Some(pending) = state.pending.remove(&operation) else {
            return Err(fdo::Error::InvalidArgs(
                "There is no pending operation with that id".into(),
            ));
        };
        finish_pending_denied(&mut state, pending, operation, "cancelled").await;
        drop(state);
        Self::pending_changed(&emitter).await.ok();
        Ok(())
    }

    async fn stop_all(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> fdo::Result<()> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let mut state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        cancel_all_pending(&mut state).await;
        drop(state);
        Self::pending_changed(&emitter).await.ok();
        Ok(())
    }

    async fn get_grants(
        &self,
        #[zbus(header)] header: Header<'_>,
    ) -> fdo::Result<Vec<OwnedValue>> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        Ok(state
            .grants
            .as_rows()
            .iter()
            .filter_map(json_to_owned)
            .collect())
    }

    async fn revoke_client(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        client: &str,
    ) -> fdo::Result<()> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let mut state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        state.grants.revoke(client);
        state.grants.save().map_err(fdo::Error::Failed)?;
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn get_enabled(&self, #[zbus(header)] header: Header<'_>) -> fdo::Result<bool> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        Ok(state.agent_enabled)
    }

    async fn set_enabled(
        &self,
        #[zbus(header)] header: Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        enabled: bool,
    ) -> fdo::Result<()> {
        let sender = header
            .sender()
            .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
            .to_string();
        let mut state = self.state.lock().await;
        require_desktop(&state, &sender)?;
        state.agent_enabled = enabled;
        if !enabled {
            cancel_all_pending(&mut state).await;
        }
        drop(state);
        Self::changed(&emitter).await.ok();
        Self::pending_changed(&emitter).await.ok();
        Ok(())
    }

    #[zbus(signal)]
    async fn changed(emitter: &SignalEmitter<'_>) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn pending_changed(emitter: &SignalEmitter<'_>) -> zbus::Result<()>;
}
