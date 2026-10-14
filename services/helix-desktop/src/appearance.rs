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
        let shell_mode = state
            .config
            .get("theme.shell_mode")
            .as_str()
            .unwrap_or("follow")
            .to_string();
        let offline = state
            .config
            .get("shell.offline_mode")
            .as_bool()
            .unwrap_or(false);
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
        map.insert(
            "wallhaven-online".into(),
            owned_bool(desktop && !offline),
        );
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
        let mode = state
            .config
            .get("theme.mode")
            .as_str()
            .unwrap_or("dark")
            .to_string();
        let apply = crate::template_apply::reapply_templates(&state.config);
        drop(state);
        if let Err(message) = apply {
            return Err(fdo::Error::Failed(
                if message.is_empty() {
                    "Appearance is not ready".into()
                } else {
                    message
                },
            ));
        }
        Self::applied(&emitter, &mode, true).await.ok();
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
            let palette = crate::palette_catalog::sync_community_catalog(&settings_toml, offline);
            let templates =
                crate::community_template_catalog::sync_community_template_catalog(&settings_toml, offline);
            (palette, templates)
        })
        .await
        .map_err(|error| fdo::Error::Failed(error.to_string()))?;
        if let Err(error) = sync_result.0 {
            tracing::warn!(%error, "community palette catalog sync failed; using cache");
        }
        if let Err(error) = sync_result.1 {
            tracing::warn!(%error, "community template catalog sync failed; using cache");
        }
        let mut state = self.state.lock().await;
        state.refresh_palette_previews();
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn save_palette(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        name: &str,
    ) -> fdo::Result<String> {
        let mut state = self.state.lock().await;
        if state.desktop.is_none() {
            return Err(fdo::Error::InvalidArgs(
                "Appearance exports require Helix desktop".into(),
            ));
        }
        drop(state);
        let source = name.to_string();
        let palette_name =
            tokio::task::spawn_blocking(move || crate::theme_palette::save_palette(&source))
            .await
            .map_err(|error| fdo::Error::Failed(error.to_string()))?
            .map_err(fdo::Error::Failed)?;
        let mut state = self.state.lock().await;
        state.config.force_reload();
        state.refresh_palette_previews();
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(palette_name)
    }

    async fn set_role(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        role: u32,
        value: &str,
    ) -> fdo::Result<String> {
        let mut state = self.state.lock().await;
        if state.desktop.is_none() {
            return Err(fdo::Error::InvalidArgs(
                "Appearance exports require Helix desktop".into(),
            ));
        }
        helix_shell_core::desktop::color_spec::validate_accent_hex(value)
            .map_err(fdo::Error::InvalidArgs)?;
        drop(state);
        let hex_color = value.to_string();
        let palette_name =
            tokio::task::spawn_blocking(move || crate::theme_palette::set_role(role, &hex_color))
                .await
                .map_err(|error| fdo::Error::Failed(error.to_string()))?
                .map_err(fdo::Error::Failed)?;
        let mut state = self.state.lock().await;
        state.config.force_reload();
        state.refresh_palette_previews();
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(palette_name)
    }

    async fn set_accent(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        color: &str,
    ) -> fdo::Result<()> {
        let state = self.state.lock().await;
        if state.desktop.is_none() {
            return Err(fdo::Error::InvalidArgs(
                "Appearance exports require Helix desktop".into(),
            ));
        }
        helix_shell_core::desktop::color_spec::validate_accent_hex(color)
            .map_err(fdo::Error::InvalidArgs)?;
        drop(state);
        let hex_color = color.to_string();
        tokio::task::spawn_blocking(move || crate::theme_palette::set_accent(&hex_color))
            .await
            .map_err(|error| fdo::Error::Failed(error.to_string()))?
            .map_err(fdo::Error::Failed)?;
        let mut state = self.state.lock().await;
        state.config.force_reload();
        state.refresh_palette_previews();
        drop(state);
        Self::changed(&emitter).await.ok();
        Ok(())
    }

    async fn set_wallpaper(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        path: &str,
    ) -> fdo::Result<()> {
        let mut state = self.state.lock().await;
        if state.desktop.is_none() {
            return Err(fdo::Error::InvalidArgs(
                "Wallpaper requires Helix desktop".into(),
            ));
        }
        if !std::path::Path::new(path).is_file() {
            return Err(fdo::Error::InvalidArgs(
                "Choose an existing wallpaper file".into(),
            ));
        }
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

    async fn set_wallhaven_api_key(&self, key: &str) -> fdo::Result<()> {
        let mut state = self.state.lock().await;
        if state.desktop.is_none() {
            return Err(fdo::Error::InvalidArgs(
                "Wallhaven settings require a Helix desktop".into(),
            ));
        }
        if key.is_empty()
            || key.len() > 256
            || !key.bytes().all(|byte| byte.is_ascii_alphanumeric())
        {
            return Err(fdo::Error::InvalidArgs(
                "Enter a valid Wallhaven API key".into(),
            ));
        }
        state
            .config
            .set_wallhaven_api_key(key)
            .map_err(fdo::Error::Failed)?;
        Ok(())
    }

    #[zbus(signal)]
    async fn changed(emitter: &SignalEmitter<'_>) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn applied(emitter: &SignalEmitter<'_>, name: &str, ok: bool) -> zbus::Result<()>;
}
