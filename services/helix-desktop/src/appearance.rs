use std::collections::HashMap;
use std::sync::Arc;

use tokio::sync::Mutex;
use zbus::fdo;
use zbus::interface;
use zbus::message::Header;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::OwnedValue;

use crate::dbus_value::{json_to_owned, owned_bool, owned_str, owned_to_json};
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

fn sender_name(header: &Header<'_>) -> fdo::Result<String> {
    Ok(header
        .sender()
        .ok_or_else(|| fdo::Error::Failed("missing sender".into()))?
        .to_string())
}

fn wallhaven_error(error: crate::wallhaven::WhError) -> fdo::Error {
    match error {
        crate::wallhaven::WhError::Invalid(message) => fdo::Error::InvalidArgs(message),
        // Like C++, runtime failures expose a fixed message, not backend detail.
        crate::wallhaven::WhError::Failed(message) => fdo::Error::Failed(message),
    }
}

/// Selects an in-process saved palette (the C++ helper does this itself) and reloads state.
fn finish_palette_save(
    state: &mut crate::state::State,
    outcome: crate::theme_palette::Outcome,
) -> fdo::Result<String> {
    use crate::theme_palette::Outcome;
    let name = match outcome {
        Outcome::Helper(name) => {
            state.config.force_reload();
            name.unwrap_or_default()
        }
        Outcome::Local(name) => {
            state
                .config
                .set("theme.source", serde_json::json!("custom"))
                .and_then(|()| {
                    state
                        .config
                        .set("theme.custom_palette", serde_json::json!(name))
                })
                .map_err(fdo::Error::Failed)?;
            name
        }
    };
    Ok(name)
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
        let key_available = desktop
            && state
                .config
                .get_json_value("wallpaper.wallhaven.api_key")
                .and_then(|value| value.as_str().map(|key| !key.trim().is_empty()))
                .unwrap_or(false);
        map.insert("wallhaven-key-available".into(), owned_bool(key_available));
        map.insert("wallhaven-online".into(), owned_bool(desktop && !offline));
        let download_directory = if desktop {
            state
                .wallhaven_context()
                .download_dir
                .to_string_lossy()
                .into_owned()
        } else {
            String::new()
        };
        map.insert(
            "wallhaven-download-directory".into(),
            owned_str(&download_directory),
        );
        map
    }

    async fn reapply(&self, #[zbus(signal_emitter)] emitter: SignalEmitter<'_>) -> fdo::Result<()> {
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
        let apply = crate::blocking::run(|| crate::template_apply::reapply_templates(&mut state));
        drop(state);
        if let Err(message) = apply {
            return Err(fdo::Error::Failed(if message.is_empty() {
                "Appearance is not ready".into()
            } else {
                message
            }));
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
            let templates = crate::community_template_catalog::sync_community_template_catalog(
                &settings_toml,
                offline,
            );
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
        let state = self.state.lock().await;
        if state.desktop.is_none() {
            return Err(fdo::Error::InvalidArgs(
                "Appearance exports require Helix desktop".into(),
            ));
        }
        let inputs = crate::theme_palette::Inputs::capture(&state.config, &state.wallpaper);
        drop(state);
        let source = name.to_string();
        let outcome = tokio::task::spawn_blocking(move || {
            crate::theme_palette::save_palette(&inputs, &source)
        })
        .await
        .map_err(|error| fdo::Error::Failed(error.to_string()))?
        .map_err(fdo::Error::Failed)?;
        let mut state = self.state.lock().await;
        let palette_name = finish_palette_save(&mut state, outcome)?;
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
        let state = self.state.lock().await;
        if state.desktop.is_none() {
            return Err(fdo::Error::InvalidArgs(
                "Appearance exports require Helix desktop".into(),
            ));
        }
        helix_config::color_spec::validate_accent_hex(value).map_err(fdo::Error::InvalidArgs)?;
        let inputs = crate::theme_palette::Inputs::capture(&state.config, &state.wallpaper);
        drop(state);
        let hex_color = value.to_string();
        let outcome = tokio::task::spawn_blocking(move || {
            crate::theme_palette::set_role(&inputs, role, &hex_color)
        })
        .await
        .map_err(|error| fdo::Error::Failed(error.to_string()))?
        .map_err(fdo::Error::Failed)?;
        let mut state = self.state.lock().await;
        let palette_name = finish_palette_save(&mut state, outcome)?;
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
        helix_config::color_spec::validate_accent_hex(color).map_err(fdo::Error::InvalidArgs)?;
        let inputs = crate::theme_palette::Inputs::capture(&state.config, &state.wallpaper);
        drop(state);
        let hex_color = color.to_string();
        let outcome = tokio::task::spawn_blocking(move || {
            crate::theme_palette::set_accent(&inputs, &hex_color)
        })
        .await
        .map_err(|error| fdo::Error::Failed(error.to_string()))?
        .map_err(fdo::Error::Failed)?;
        let mut state = self.state.lock().await;
        finish_palette_save(&mut state, outcome)?;
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

    async fn search_wallhaven(
        &self,
        #[zbus(header)] header: Header<'_>,
        args: HashMap<String, OwnedValue>,
    ) -> fdo::Result<u64> {
        let sender = sender_name(&header)?;
        let mut object = serde_json::Map::new();
        for (key, value) in &args {
            if let Some(json) = owned_to_json(value) {
                object.insert(key.clone(), json);
            }
        }
        let state = self.state.lock().await;
        let context = state.wallhaven_context();
        state
            .wallhaven
            .search(&context, &serde_json::Value::Object(object), &sender)
            .map_err(wallhaven_error)
    }

    async fn get_wallhaven(
        &self,
        #[zbus(header)] header: Header<'_>,
        operation: u64,
    ) -> fdo::Result<HashMap<String, OwnedValue>> {
        let sender = sender_name(&header)?;
        let state = self.state.lock().await;
        let result = state
            .wallhaven
            .result(operation, &sender)
            .map_err(wallhaven_error)?;
        let mut map = HashMap::new();
        if let Some(object) = result.as_object() {
            for (key, value) in object {
                if let Some(owned) = json_to_owned(value) {
                    map.insert(key.clone(), owned);
                }
            }
        }
        Ok(map)
    }

    async fn download_wallhaven(
        &self,
        #[zbus(header)] header: Header<'_>,
        operation: u64,
        id: &str,
    ) -> fdo::Result<u64> {
        let sender = sender_name(&header)?;
        let state = self.state.lock().await;
        let context = state.wallhaven_context();
        state
            .wallhaven
            .download(&context, operation, id, &sender)
            .map_err(wallhaven_error)
    }

    async fn cancel_wallhaven(
        &self,
        #[zbus(header)] header: Header<'_>,
        operation: u64,
    ) -> fdo::Result<()> {
        let sender = sender_name(&header)?;
        let state = self.state.lock().await;
        state
            .wallhaven
            .cancel(operation, &sender)
            .map_err(wallhaven_error)
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
