//! Coordinates NetworkManager / BlueZ system-bus agents with session-bus Prompt signals.

use std::collections::HashMap;
use std::sync::Mutex;

use tokio::sync::{oneshot, Mutex as AsyncMutex};
use zbus::zvariant::OwnedValue;
use zbus::Connection;

use crate::bluetooth_support::BLUETOOTH_INTERFACE;
use crate::dbus_value::{owned_str, owned_u64};
use crate::network_support::NETWORK_INTERFACE;
use crate::network_support::OBJECT_PATH as DESKTOP_PATH;

const NM_ALLOW_INTERACTION: u32 = 0x1;
const WIRELESS_SETTING: &str = "802-11-wireless";
const WIRELESS_SECURITY_SETTING: &str = "802-11-wireless-security";
const ENTERPRISE_SETTING: &str = "802-1x";
const PSK_KEY: &str = "psk";
const EAP_PASSWORD_KEY: &str = "password";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BluetoothPairingKind {
    None = 0,
    PinCode = 1,
    Passkey = 2,
    DisplayPinCode = 3,
    DisplayPasskey = 4,
    Confirm = 5,
    Authorize = 6,
    AuthorizeService = 7,
}

#[derive(Debug)]
enum NmWait {
    Secret(oneshot::Sender<Result<String, ()>>),
}

#[derive(Debug)]
enum BtWait {
    String(oneshot::Sender<Result<String, ()>>),
    Uint(oneshot::Sender<Result<u32, ()>>),
    Void(oneshot::Sender<Result<(), ()>>),
}

#[derive(Debug, Clone)]
struct BtPrompt {
    kind: BluetoothPairingKind,
    device: String,
    passkey: u32,
    pin: String,
    entered: u16,
}

#[derive(Debug)]
struct BtPending {
    prompt: BtPrompt,
    wait: BtWait,
}

#[derive(Debug)]
struct NmPending {
    setting_name: String,
    wait: NmWait,
}

pub struct AgentHub {
    session: AsyncMutex<Option<Connection>>,
    network_owner: Mutex<Option<String>>,
    bluetooth_owner: Mutex<Option<String>>,
    nm_pending: Mutex<Option<NmPending>>,
    bt_pending: Mutex<Option<BtPending>>,
}

impl AgentHub {
    pub fn new() -> Self {
        Self {
            session: AsyncMutex::new(None),
            network_owner: Mutex::new(None),
            bluetooth_owner: Mutex::new(None),
            nm_pending: Mutex::new(None),
            bt_pending: Mutex::new(None),
        }
    }

    pub async fn set_session(&self, connection: Connection) {
        *self.session.lock().await = Some(connection);
    }

    pub fn network_owner(&self) -> Option<String> {
        self.network_owner.lock().expect("network_owner").clone()
    }

    pub fn set_network_owner(&self, owner: String) {
        *self.network_owner.lock().expect("network_owner") = Some(owner);
    }

    pub fn clear_network_owner(&self) {
        *self.network_owner.lock().expect("network_owner") = None;
    }

    pub fn bluetooth_owner(&self) -> Option<String> {
        self.bluetooth_owner.lock().expect("bluetooth_owner").clone()
    }

    pub fn set_bluetooth_owner(&self, owner: String) {
        *self.bluetooth_owner.lock().expect("bluetooth_owner") = Some(owner);
    }

    pub fn clear_bluetooth_owner(&self) {
        *self.bluetooth_owner.lock().expect("bluetooth_owner") = None;
    }

    pub fn on_peer_disconnected(&self, unique_name: &str) {
        if self.network_owner() == Some(unique_name.to_string()) {
            self.cancel_nm_secret("client disconnected");
            self.clear_network_owner();
        }
        if self.bluetooth_owner() == Some(unique_name.to_string()) {
            self.cancel_bluetooth_pending();
            self.clear_bluetooth_owner();
        }
    }

    pub fn nm_has_pending(&self) -> bool {
        self.nm_pending.lock().expect("nm_pending").is_some()
    }

    pub fn bt_has_pending(&self) -> bool {
        self.bt_pending
            .lock()
            .expect("bt_pending")
            .is_some()
    }

    pub fn bt_pending_kind(&self) -> BluetoothPairingKind {
        self.bt_pending
            .lock()
            .expect("bt_pending")
            .as_ref()
            .map(|pending| pending.prompt.kind)
            .unwrap_or(BluetoothPairingKind::None)
    }

    pub fn cancel_nm_secret(&self, reason: &str) {
        let mut pending = self.nm_pending.lock().expect("nm_pending");
        if let Some(request) = pending.take() {
            if let NmWait::Secret(sender) = request.wait {
                let _ = sender.send(Err(()));
            }
        }
        tracing::debug!(reason, "canceled NM secret request");
    }

    pub fn submit_network_reply(&self, response: &str, cancel: bool) -> Result<(), &'static str> {
        if cancel {
            self.cancel_nm_secret("user canceled");
            return Ok(());
        }
        if !self.nm_has_pending() {
            return Err("No credential request for this client");
        }
        let mut pending = self.nm_pending.lock().expect("nm_pending");
        if let Some(request) = pending.take() {
            if let NmWait::Secret(sender) = request.wait {
                let _ = sender.send(Ok(response.to_string()));
            }
        }
        Ok(())
    }

    pub fn submit_bluetooth_reply(&self, response: &str, cancel: bool) -> Result<(), &'static str> {
        if !self.bt_has_pending() {
            return Err("No pairing request for this client");
        }
        if cancel {
            self.cancel_bluetooth_pending();
            return Ok(());
        }
        let kind = self.bt_pending_kind();
        match kind {
            BluetoothPairingKind::PinCode => self.complete_bt_string(response.to_string()),
            BluetoothPairingKind::Passkey => {
                if response.is_empty()
                    || response.len() > 6
                    || !response.chars().all(|ch| ch.is_ascii_digit())
                {
                    return Err("Passkey must contain at most six digits");
                }
                let value = response.parse::<u32>().unwrap_or(0);
                self.complete_bt_uint(value);
            }
            BluetoothPairingKind::DisplayPinCode
            | BluetoothPairingKind::DisplayPasskey
            | BluetoothPairingKind::Confirm
            | BluetoothPairingKind::Authorize
            | BluetoothPairingKind::AuthorizeService => self.complete_bt_void(),
            BluetoothPairingKind::None => return Err("No pairing request for this client"),
        }
        Ok(())
    }

    fn complete_bt_string(&self, value: String) {
        let mut pending = self.bt_pending.lock().expect("bt_pending");
        if let Some(request) = pending.take() {
            if let BtWait::String(sender) = request.wait {
                let _ = sender.send(Ok(value));
            }
        }
    }

    fn complete_bt_uint(&self, value: u32) {
        let mut pending = self.bt_pending.lock().expect("bt_pending");
        if let Some(request) = pending.take() {
            if let BtWait::Uint(sender) = request.wait {
                let _ = sender.send(Ok(value));
            }
        }
    }

    fn complete_bt_void(&self) {
        let mut pending = self.bt_pending.lock().expect("bt_pending");
        if let Some(request) = pending.take() {
            if let BtWait::Void(sender) = request.wait {
                let _ = sender.send(Ok(()));
            }
        }
    }

    pub fn cancel_bluetooth_pending(&self) {
        let mut pending = self.bt_pending.lock().expect("bt_pending");
        if let Some(request) = pending.take() {
            match request.wait {
                BtWait::String(sender) => {
                    let _ = sender.send(Err(()));
                }
                BtWait::Uint(sender) => {
                    let _ = sender.send(Err(()));
                }
                BtWait::Void(sender) => {
                    let _ = sender.send(Err(()));
                }
            }
        }
    }

    pub fn nm_flags_allow_interaction(flags: u32) -> bool {
        (flags & NM_ALLOW_INTERACTION) != 0
    }

    pub fn nm_setting_supported(setting_name: &str) -> bool {
        setting_name == WIRELESS_SECURITY_SETTING || setting_name == ENTERPRISE_SETTING
    }

    pub fn nm_secret_key(setting_name: &str) -> &'static str {
        if setting_name == ENTERPRISE_SETTING {
            EAP_PASSWORD_KEY
        } else {
            PSK_KEY
        }
    }

    pub fn extract_ssid(connection: &HashMap<String, HashMap<String, OwnedValue>>) -> String {
        let wifi = connection.get(WIRELESS_SETTING);
        let ssid_value = wifi.and_then(|settings| settings.get("ssid"));
        match ssid_value.map(|value| &**value) {
            Some(zbus::zvariant::Value::Array(bytes)) => {
                let mut out = Vec::new();
                for item in bytes.iter() {
                    if let zbus::zvariant::Value::U8(byte) = item {
                        out.push(*byte);
                    }
                }
                String::from_utf8_lossy(&out).to_string()
            }
            Some(zbus::zvariant::Value::Str(text)) => text.to_string(),
            _ => String::new(),
        }
    }

    pub async fn begin_nm_secret(
        &self,
        ssid: &str,
        setting_name: String,
    ) -> Result<String, NmSecretOutcome> {
        if self.nm_pending.lock().expect("nm_pending").is_some() {
            return Err(NmSecretOutcome::NoSecrets("another secret request is already pending"));
        }
        let destination = self
            .network_owner()
            .ok_or(NmSecretOutcome::NoSecrets("no UI handler"))?;
        let (tx, rx) = oneshot::channel();
        *self.nm_pending.lock().expect("nm_pending") = Some(NmPending {
            setting_name: setting_name.clone(),
            wait: NmWait::Secret(tx),
        });
        if let Err(error) = self
            .emit_network_prompt(&destination, ssid, &setting_name)
            .await
        {
            self.cancel_nm_secret("prompt failed");
            tracing::warn!(%error, "network Prompt emit failed");
            return Err(NmSecretOutcome::UserCanceled("prompt failed"));
        }
        match rx.await {
            Ok(Ok(secret)) => Ok(secret),
            _ => Err(NmSecretOutcome::UserCanceled("user canceled")),
        }
    }

    pub fn build_nm_secrets(
        setting_name: &str,
        secret: &str,
    ) -> HashMap<String, HashMap<String, OwnedValue>> {
        let mut inner = HashMap::new();
        inner.insert(
            Self::nm_secret_key(setting_name).to_string(),
            OwnedValue::try_from(zbus::zvariant::Value::from(secret))
                .expect("secret value"),
        );
        let mut outer = HashMap::new();
        outer.insert(setting_name.to_string(), inner);
        outer
    }

    async fn emit_network_prompt(
        &self,
        destination: &str,
        ssid: &str,
        setting: &str,
    ) -> zbus::Result<()> {
        let session = self.session.lock().await;
        let conn = session
            .as_ref()
            .ok_or_else(|| zbus::Error::Failure("session bus not ready".into()))?;
        let mut map = HashMap::new();
        map.insert("ssid".to_string(), owned_str(ssid));
        map.insert("setting".to_string(), owned_str(setting));
        conn.emit_signal(
            Some(destination),
            DESKTOP_PATH,
            NETWORK_INTERFACE,
            "Prompt",
            &map,
        )
        .await?;
        Ok(())
    }

    async fn emit_bluetooth_prompt(&self, destination: &str, prompt: &BtPrompt) -> zbus::Result<()> {
        let session = self.session.lock().await;
        let conn = session
            .as_ref()
            .ok_or_else(|| zbus::Error::Failure("session bus not ready".into()))?;
        let mut map = HashMap::new();
        map.insert("kind".to_string(), owned_u64(prompt.kind as u64));
        map.insert("device".to_string(), owned_str(&prompt.device));
        map.insert("passkey".to_string(), owned_u64(prompt.passkey as u64));
        map.insert("pin".to_string(), owned_str(&prompt.pin));
        map.insert("entered".to_string(), owned_u64(prompt.entered as u64));
        conn.emit_signal(
            Some(destination),
            DESKTOP_PATH,
            BLUETOOTH_INTERFACE,
            "Prompt",
            &map,
        )
        .await?;
        Ok(())
    }

    fn begin_bt(
        &self,
        prompt: BtPrompt,
        wait: BtWait,
    ) -> Result<(), BtAgentOutcome> {
        if self.bt_pending.lock().expect("bt_pending").is_some() {
            return Err(BtAgentOutcome::Rejected);
        }
        if self.bluetooth_owner().is_none() {
            return Err(BtAgentOutcome::Rejected);
        }
        *self.bt_pending.lock().expect("bt_pending") = Some(BtPending { prompt, wait });
        Ok(())
    }

    fn bt_prompt_snapshot(&self) -> Option<BtPrompt> {
        self.bt_pending
            .lock()
            .expect("bt_pending")
            .as_ref()
            .map(|pending| pending.prompt.clone())
    }

    async fn emit_current_bt_prompt(&self) -> Result<(), BtAgentOutcome> {
        let owner = self.bluetooth_owner().ok_or(BtAgentOutcome::Rejected)?;
        let prompt = self.bt_prompt_snapshot().ok_or(BtAgentOutcome::Rejected)?;
        if let Err(error) = self.emit_bluetooth_prompt(&owner, &prompt).await {
            self.cancel_bluetooth_pending();
            tracing::warn!(%error, "bluetooth Prompt emit failed");
            return Err(BtAgentOutcome::Canceled);
        }
        Ok(())
    }

    pub async fn bt_request_pin_code(&self, device: String) -> Result<String, BtAgentOutcome> {
        let (tx, rx) = oneshot::channel();
        self.begin_bt(
            BtPrompt {
                kind: BluetoothPairingKind::PinCode,
                device,
                passkey: 0,
                pin: String::new(),
                entered: 0,
            },
            BtWait::String(tx),
        )?;
        self.emit_current_bt_prompt().await?;
        match rx.await {
            Ok(Ok(pin)) => Ok(pin),
            _ => Err(BtAgentOutcome::Canceled),
        }
    }

    pub async fn bt_display_pin_code(
        &self,
        device: String,
        pincode: String,
    ) -> Result<(), BtAgentOutcome> {
        let (tx, rx) = oneshot::channel();
        self.begin_bt(
            BtPrompt {
                kind: BluetoothPairingKind::DisplayPinCode,
                device,
                passkey: 0,
                pin: pincode,
                entered: 0,
            },
            BtWait::Void(tx),
        )?;
        self.emit_current_bt_prompt().await?;
        match rx.await {
            Ok(Ok(())) => Ok(()),
            _ => Err(BtAgentOutcome::Canceled),
        }
    }

    pub async fn bt_request_passkey(&self, device: String) -> Result<u32, BtAgentOutcome> {
        let (tx, rx) = oneshot::channel();
        self.begin_bt(
            BtPrompt {
                kind: BluetoothPairingKind::Passkey,
                device,
                passkey: 0,
                pin: String::new(),
                entered: 0,
            },
            BtWait::Uint(tx),
        )?;
        self.emit_current_bt_prompt().await?;
        match rx.await {
            Ok(Ok(passkey)) => Ok(passkey),
            _ => Err(BtAgentOutcome::Canceled),
        }
    }

    pub async fn bt_display_passkey(
        &self,
        device: String,
        passkey: u32,
        entered: u16,
    ) -> Result<(), BtAgentOutcome> {
        let updated_prompt = {
            let mut slot = self.bt_pending.lock().expect("bt_pending");
            if let Some(pending) = slot.as_mut() {
                if pending.prompt.kind == BluetoothPairingKind::DisplayPasskey
                    && pending.prompt.device == device
                {
                    pending.prompt.passkey = passkey;
                    pending.prompt.entered = entered;
                    Some(pending.prompt.clone())
                } else {
                    None
                }
            } else {
                None
            }
        };
        if let Some(prompt) = updated_prompt {
            let owner = self.bluetooth_owner().unwrap_or_default();
            if let Err(error) = self.emit_bluetooth_prompt(&owner, &prompt).await {
                tracing::warn!(%error, "bluetooth DisplayPasskey re-prompt failed");
            }
            return Ok(());
        }
        let (tx, rx) = oneshot::channel();
        self.begin_bt(
            BtPrompt {
                kind: BluetoothPairingKind::DisplayPasskey,
                device,
                passkey,
                pin: String::new(),
                entered,
            },
            BtWait::Void(tx),
        )?;
        self.emit_current_bt_prompt().await?;
        match rx.await {
            Ok(Ok(())) => Ok(()),
            _ => Err(BtAgentOutcome::Canceled),
        }
    }

    pub async fn bt_request_confirmation(
        &self,
        device: String,
        passkey: u32,
    ) -> Result<(), BtAgentOutcome> {
        let (tx, rx) = oneshot::channel();
        self.begin_bt(
            BtPrompt {
                kind: BluetoothPairingKind::Confirm,
                device,
                passkey,
                pin: String::new(),
                entered: 0,
            },
            BtWait::Void(tx),
        )?;
        self.emit_current_bt_prompt().await?;
        match rx.await {
            Ok(Ok(())) => Ok(()),
            _ => Err(BtAgentOutcome::Canceled),
        }
    }

    pub async fn bt_request_authorization(&self, device: String) -> Result<(), BtAgentOutcome> {
        let (tx, rx) = oneshot::channel();
        self.begin_bt(
            BtPrompt {
                kind: BluetoothPairingKind::Authorize,
                device,
                passkey: 0,
                pin: String::new(),
                entered: 0,
            },
            BtWait::Void(tx),
        )?;
        self.emit_current_bt_prompt().await?;
        match rx.await {
            Ok(Ok(())) => Ok(()),
            _ => Err(BtAgentOutcome::Canceled),
        }
    }

    pub async fn bt_authorize_service(
        &self,
        device: String,
        _uuid: String,
    ) -> Result<(), BtAgentOutcome> {
        let (tx, rx) = oneshot::channel();
        self.begin_bt(
            BtPrompt {
                kind: BluetoothPairingKind::AuthorizeService,
                device,
                passkey: 0,
                pin: String::new(),
                entered: 0,
            },
            BtWait::Void(tx),
        )?;
        self.emit_current_bt_prompt().await?;
        match rx.await {
            Ok(Ok(())) => Ok(()),
            _ => Err(BtAgentOutcome::Canceled),
        }
    }

    pub fn bt_on_cancel(&self) {
        self.cancel_bluetooth_pending();
    }
}

#[derive(Debug)]
pub enum NmSecretOutcome {
    NoSecrets(&'static str),
    UserCanceled(&'static str),
}

#[derive(Debug, Clone, Copy)]
pub enum BtAgentOutcome {
    Rejected,
    Canceled,
}
