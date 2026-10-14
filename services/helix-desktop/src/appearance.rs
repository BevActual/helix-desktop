use std::collections::HashMap;
use std::sync::Arc;

use tokio::sync::Mutex;
use zbus::fdo;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::OwnedValue;
use zbus::interface;

use crate::dbus_value::{json_to_owned, owned_bool, owned_str};
use crate::state::State;

pub struct Appearance {
    pub state: Arc<Mutex<State>>,
}

fn previews_value(previews: &[serde_json::Value]) -> OwnedValue {
    json_to_owned(&serde_json::Value::Array(previews.to_vec())).unwrap_or_else(|| {
        OwnedValue::try_from(zbus::zvariant::Value::from(Vec::<OwnedValue>::new()))
            .expect("empty array")
    })
}

#[interface(name = "org.helix.Desktop.Appearance1")]
impl Appearance {
    async fn get_snapshot(&self) -> HashMap<String, OwnedValue> {
        let state = self.state.lock().await;
        let mode = state
            .config
            .get("theme.mode")
            .as_str()
            .unwrap_or("dark")
            .to_string();
        let shell_mode = "follow".to_string();
        let desktop = state.desktop.is_some();
        let mut map = HashMap::new();
        map.insert("available".into(), owned_bool(true));
        map.insert("helix-desktop".into(), owned_bool(desktop));
        map.insert("mode".into(), owned_str(&mode));
        map.insert("shell-mode".into(), owned_str(&shell_mode));
        map.insert(
            "wallpaper".into(),
            owned_str(if desktop { &state.wallpaper } else { "" }),
        );
        map.insert(
            "palette-previews".into(),
            previews_value(&state.palette_previews),
        );
        map.insert("wallhaven-key-available".into(), owned_bool(false));
        map.insert("wallhaven-online".into(), owned_bool(false));
        map.insert("wallhaven-download-directory".into(), owned_str(""));
        map
    }

    async fn reapply(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> fdo::Result<()> {
        let mut state = self.state.lock().await;
        if state.desktop.is_none() {
            return Err(fdo::Error::InvalidArgs(
                "Appearance exports require Helix desktop".into(),
            ));
        }
        state.refresh_palette_previews();
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn refresh_palettes(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> fdo::Result<()> {
        let (settings_toml, offline) = {
            let state = self.state.lock().await;
            (
                state.config.paths().settings_toml.clone(),
                state
                    .config
                    .get("shell.offline_mode")
                    .as_bool()
                    .unwrap_or(false),
            )
        };
        let sync_result = tokio::task::spawn_blocking(move || {
            crate::palette_catalog::sync_community_catalog(&settings_toml, offline)
        })
        .await
        .map_err(|error| fdo::Error::Failed(error.to_string()))?;
        if let Err(error) = sync_result {
            tracing::warn!(%error, "community palette catalog sync failed; using cache");
        }
        let mut state = self.state.lock().await;
        state.refresh_palette_previews();
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn save_palette(&self, _name: &str) -> fdo::Result<String> {
        Err(fdo::Error::Failed(
            "SavePalette not implemented in Rust helix-desktop yet".into(),
        ))
    }

    async fn set_role(&self, _role: u32, _value: &str) -> fdo::Result<String> {
        Err(fdo::Error::Failed(
            "SetRole not implemented in Rust helix-desktop yet".into(),
        ))
    }

    async fn set_accent(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        _color: &str,
    ) -> fdo::Result<()> {
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn set_wallpaper(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        path: &str,
    ) -> fdo::Result<()> {
        let mut state = self.state.lock().await;
        state.wallpaper = path.to_string();
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn search_wallhaven(&self, _args: HashMap<String, OwnedValue>) -> fdo::Result<u64> {
        Err(fdo::Error::Failed("Wallhaven not implemented yet".into()))
    }

    async fn get_wallhaven(&self, _operation: u64) -> fdo::Result<HashMap<String, OwnedValue>> {
        Err(fdo::Error::Failed("Wallhaven not implemented yet".into()))
    }

    async fn download_wallhaven(&self, _operation: u64, _id: &str) -> fdo::Result<u64> {
        Err(fdo::Error::Failed("Wallhaven not implemented yet".into()))
    }

    async fn cancel_wallhaven(&self, _operation: u64) -> fdo::Result<()> {
        Ok(())
    }

    async fn set_wallhaven_api_key(&self, _key: &str) -> fdo::Result<()> {
        Ok(())
    }

    #[zbus(signal)]
    async fn changed(emitter: &SignalEmitter<'_>) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn applied(emitter: &SignalEmitter<'_>, name: &str, ok: bool) -> zbus::Result<()>;
}
