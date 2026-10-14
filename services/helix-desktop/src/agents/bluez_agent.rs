//! BlueZ Agent1 on the system bus.

use std::sync::Arc;

use zbus::fdo;
use zbus::interface;

use super::hub::{AgentHub, BtAgentOutcome};

pub const OBJECT_PATH: &str = "/org/helix/BluetoothAgent";
const AGENT_MANAGER_INTERFACE: &str = "org.bluez.AgentManager1";
const CAPABILITY: &str = "KeyboardDisplay";

const BLUEZ_BUS: &str = "org.bluez";
const BLUEZ_ROOT: &str = "/org/bluez";

#[derive(Debug, zbus::DBusError)]
#[zbus(prefix = "org.bluez.Error")]
enum BluezAgentError {
    Rejected,
    Canceled,
}

pub struct BluezAgent {
    pub hub: Arc<AgentHub>,
}

#[interface(name = "org.bluez.Agent1")]
impl BluezAgent {
    async fn release(&self) -> fdo::Result<()> {
        Ok(())
    }

    async fn request_pin_code(
        &self,
        device: zbus::zvariant::ObjectPath<'_>,
    ) -> Result<String, BluezAgentError> {
        match self.hub.bt_request_pin_code(device.to_string()).await {
            Ok(pin) => Ok(pin),
            Err(BtAgentOutcome::Rejected) => Err(BluezAgentError::Rejected.into()),
            Err(BtAgentOutcome::Canceled) => Err(BluezAgentError::Canceled.into()),
        }
    }

    async fn display_pin_code(
        &self,
        device: zbus::zvariant::ObjectPath<'_>,
        pincode: String,
    ) -> Result<(), BluezAgentError> {
        match self
            .hub
            .bt_display_pin_code(device.to_string(), pincode)
            .await
        {
            Ok(()) => Ok(()),
            Err(BtAgentOutcome::Rejected) => Err(BluezAgentError::Rejected.into()),
            Err(BtAgentOutcome::Canceled) => Err(BluezAgentError::Canceled.into()),
        }
    }

    async fn request_passkey(
        &self,
        device: zbus::zvariant::ObjectPath<'_>,
    ) -> Result<u32, BluezAgentError> {
        match self.hub.bt_request_passkey(device.to_string()).await {
            Ok(passkey) => Ok(passkey),
            Err(BtAgentOutcome::Rejected) => Err(BluezAgentError::Rejected.into()),
            Err(BtAgentOutcome::Canceled) => Err(BluezAgentError::Canceled.into()),
        }
    }

    async fn display_passkey(
        &self,
        device: zbus::zvariant::ObjectPath<'_>,
        passkey: u32,
        entered: u16,
    ) -> Result<(), BluezAgentError> {
        match self
            .hub
            .bt_display_passkey(device.to_string(), passkey, entered)
            .await
        {
            Ok(()) => Ok(()),
            Err(BtAgentOutcome::Rejected) => Err(BluezAgentError::Rejected.into()),
            Err(BtAgentOutcome::Canceled) => Err(BluezAgentError::Canceled.into()),
        }
    }

    async fn request_confirmation(
        &self,
        device: zbus::zvariant::ObjectPath<'_>,
        passkey: u32,
    ) -> Result<(), BluezAgentError> {
        match self
            .hub
            .bt_request_confirmation(device.to_string(), passkey)
            .await
        {
            Ok(()) => Ok(()),
            Err(BtAgentOutcome::Rejected) => Err(BluezAgentError::Rejected.into()),
            Err(BtAgentOutcome::Canceled) => Err(BluezAgentError::Canceled.into()),
        }
    }

    async fn request_authorization(
        &self,
        device: zbus::zvariant::ObjectPath<'_>,
    ) -> Result<(), BluezAgentError> {
        match self.hub.bt_request_authorization(device.to_string()).await {
            Ok(()) => Ok(()),
            Err(BtAgentOutcome::Rejected) => Err(BluezAgentError::Rejected.into()),
            Err(BtAgentOutcome::Canceled) => Err(BluezAgentError::Canceled.into()),
        }
    }

    async fn authorize_service(
        &self,
        device: zbus::zvariant::ObjectPath<'_>,
        uuid: String,
    ) -> Result<(), BluezAgentError> {
        match self
            .hub
            .bt_authorize_service(device.to_string(), uuid)
            .await
        {
            Ok(()) => Ok(()),
            Err(BtAgentOutcome::Rejected) => Err(BluezAgentError::Rejected.into()),
            Err(BtAgentOutcome::Canceled) => Err(BluezAgentError::Canceled.into()),
        }
    }

    async fn cancel(&self) -> fdo::Result<()> {
        self.hub.bt_on_cancel();
        Ok(())
    }
}

pub async fn register(connection: &zbus::Connection) -> zbus::Result<()> {
    let proxy = zbus::Proxy::new(
        connection,
        BLUEZ_BUS,
        BLUEZ_ROOT,
        AGENT_MANAGER_INTERFACE,
    )
    .await?;
    proxy
        .call::<_, _, ()>("RegisterAgent", &(OBJECT_PATH, CAPABILITY))
        .await?;
    proxy
        .call::<_, _, ()>("RequestDefaultAgent", &(OBJECT_PATH,))
        .await?;
    tracing::info!(OBJECT_PATH, "registered BlueZ agent");
    Ok(())
}

pub async fn unregister(connection: &zbus::Connection) {
    let proxy = zbus::Proxy::new(
        connection,
        BLUEZ_BUS,
        BLUEZ_ROOT,
        AGENT_MANAGER_INTERFACE,
    )
    .await;
    if let Ok(proxy) = proxy {
        let _ = proxy
            .call::<_, _, ()>("UnregisterAgent", &(OBJECT_PATH,))
            .await;
    }
}
