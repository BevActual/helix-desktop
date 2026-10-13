//! `org.helix.Shell1` (protocols/dbus/org.helix.Shell.xml): lets a key binding or another program open the shell's panels
//! and hand a request to the desktop assistant. Opening UI needs no grant; `AssistantSubmit` only starts the same path as
//! typing in the assistant panel — Agent Surface still gates what runs.

use std::sync::Arc;

use helix_shell_core::apps::assistant::MAX_REQUEST_CHARS;
use tauri::AppHandle;
use zbus::zvariant::{OwnedValue, Value};

use crate::{close_panel, open_panel, shell_state, submit_request};

pub const BUS_NAME: &str = "org.helix.Shell";
pub const OBJECT_PATH: &str = "/org/helix/Shell";
/// The methods this object serves. A test checks them against the XML contract.
#[cfg(test)]
pub const METHODS: &[&str] = &[
    "OpenPanel",
    "TogglePanel",
    "ClosePanel",
    "AssistantSubmit",
    "GetStatus",
    "ToggleDoNotDisturb",
    "ToggleNightLight",
    "ToggleDarkTheme",
    "AdjustVolume",
    "ToggleMute",
    "AdjustBrightness",
    "PromptAdminPassword",
    "BarShow",
    "BarHide",
    "BarToggle",
];

struct Control {
    app: AppHandle,
}

/// Local surface panel id (panels / monolith). On the bar this is always empty — use [`current_panel`].
pub(crate) fn local_panel(app: &AppHandle) -> Option<String> {
    shell_state(app)
        .lock()
        .expect("shell state")
        .surface
        .open_panel_id()
        .map(str::to_string)
}

/// Open panel id as seen by Shell1 / agent status. Bar queries panels over IPC.
pub(crate) async fn current_panel(app: &AppHandle) -> Option<String> {
    if crate::role::ShellRole::from_env().forwards_panels() {
        match crate::panels_bridge::open_status().await {
            Ok(panel) => panel,
            Err(error) => {
                eprintln!("PANELS status: {error}");
                None
            }
        }
    } else {
        local_panel(app)
    }
}

/// Runs `work` on the main thread, where GTK must be touched.
pub(crate) fn on_main(
    app: &AppHandle,
    work: impl FnOnce(&AppHandle) + Send + 'static,
) -> zbus::fdo::Result<()> {
    let runner = app.clone();
    app.run_on_main_thread(move || work(&runner))
        .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))
}

/// An id the surface does not know is an error to the caller instead of a silent no-op.
fn known(id: &str) -> zbus::fdo::Result<()> {
    helix_shell_core::shell_surface::PANELS
        .contains(&id)
        .then_some(())
        .ok_or_else(|| zbus::fdo::Error::InvalidArgs(format!("unknown panel {id:?}")))
}

#[zbus::interface(name = "org.helix.Shell1")]
impl Control {
    async fn open_panel(&self, id: String) -> zbus::fdo::Result<()> {
        known(&id)?;
        on_main(&self.app, move |app| open_panel(app, &id))
    }

    async fn toggle_panel(&self, id: String) -> zbus::fdo::Result<()> {
        known(&id)?;
        let is_open = current_panel(&self.app).await.as_deref() == Some(id.as_str());
        on_main(&self.app, move |app| {
            if is_open {
                close_panel(app);
            } else {
                open_panel(app, &id);
            }
        })
    }

    async fn toggle_do_not_disturb(&self) {
        crate::notify::toggle_dnd(&self.app).await;
    }

    async fn toggle_night_light(&self) {
        crate::toggle_night_light(&self.app).await;
    }

    /// The volume keys: change by `delta` percentage points (and show the indicator).
    async fn adjust_volume(&self, delta: i32) {
        crate::adjust_volume(&self.app, delta.clamp(-100, 100)).await;
    }

    async fn toggle_mute(&self) {
        crate::toggle_mute(&self.app).await;
    }

    async fn adjust_brightness(&self, delta: i32) {
        crate::adjust_brightness(&self.app, delta.clamp(-100, 100)).await;
    }

    async fn toggle_dark_theme(&self) {
        crate::toggle_dark_theme(&self.app).await;
    }

    async fn bar_show(&self) -> zbus::fdo::Result<()> {
        if !crate::role::ShellRole::from_env().claims_shell1() {
            return Err(zbus::fdo::Error::Failed(
                "bar visibility is served by the bar process".into(),
            ));
        }
        crate::bar::set_visible(&self.app, true)
            .map_err(zbus::fdo::Error::Failed)
    }

    async fn bar_hide(&self) -> zbus::fdo::Result<()> {
        if !crate::role::ShellRole::from_env().claims_shell1() {
            return Err(zbus::fdo::Error::Failed(
                "bar visibility is served by the bar process".into(),
            ));
        }
        crate::bar::set_visible(&self.app, false)
            .map_err(zbus::fdo::Error::Failed)
    }

    async fn bar_toggle(&self) -> zbus::fdo::Result<()> {
        if !crate::role::ShellRole::from_env().claims_shell1() {
            return Err(zbus::fdo::Error::Failed(
                "bar visibility is served by the bar process".into(),
            ));
        }
        crate::bar::toggle(&self.app).map_err(zbus::fdo::Error::Failed)
    }

    async fn close_panel(&self) -> zbus::fdo::Result<()> {
        on_main(&self.app, close_panel)
    }

    /// Opens Mobius Act and submits `request` (Mobius Ask Mobius / Caps Lock, …).
    async fn assistant_submit(&self, request: String) -> zbus::fdo::Result<()> {
        let trimmed = request.trim_matches(|c: char| c.is_ascii_whitespace());
        if trimmed.is_empty() {
            return Err(zbus::fdo::Error::InvalidArgs("request is empty".into()));
        }
        if trimmed.len() > MAX_REQUEST_CHARS {
            return Err(zbus::fdo::Error::InvalidArgs(format!(
                "request is longer than {MAX_REQUEST_CHARS} characters"
            )));
        }
        if trimmed.chars().any(|c| c.is_control() && c != '\n' && c != '\t') {
            return Err(zbus::fdo::Error::InvalidArgs(
                "request contains control characters".into(),
            ));
        }
        let request = trimmed.to_string();
        let app = self.app.clone();
        // Bar role forwards Act to the panels process; never open Overlay in-process on the bar.
        tauri::async_runtime::spawn(async move {
            if crate::role::ShellRole::from_env().forwards_panels() {
                match crate::panels_bridge::assistant_submit(&request).await {
                    Ok(true) => {}
                    Ok(false) => eprintln!(
                        "PANELS assistant_submit missed (no in-process Overlay on bar)"
                    ),
                    Err(error) => eprintln!("PANELS assistant_submit: {error}"),
                }
                return;
            }
            let _ = on_main(&app, move |app| {
                open_panel(app, "assistant");
                submit_request(app, &request);
            });
        });
        Ok(())
    }

    /// Used by `helix-askpass` (`SUDO_ASKPASS`) for graphical `sudo -A`.
    async fn prompt_admin_password(&self, prompt: String) -> zbus::fdo::Result<String> {
        if !crate::role::ShellRole::from_env().claims_shell1() {
            return Err(zbus::fdo::Error::Failed(
                "admin prompts are served by the bar process".into(),
            ));
        }
        match crate::admin_auth::prompt(self.app.clone(), prompt).await {
            Ok(secret) => Ok(secret),
            Err(error) if error == "cancelled" => {
                Err(zbus::fdo::Error::AccessDenied("cancelled".into()))
            }
            Err(error) => Err(zbus::fdo::Error::Failed(error)),
        }
    }

    async fn get_status(&self) -> std::collections::HashMap<String, OwnedValue> {
        let panel = current_panel(&self.app).await.unwrap_or_default();
        let mut status = std::collections::HashMap::new();
        if let Ok(value) = OwnedValue::try_from(Value::from(panel)) {
            status.insert("panel".to_string(), value);
        }
        status
    }
}

/// Claims the bus name and serves the object for as long as the shell runs. A second shell instance cannot take the name.
pub fn serve(app: AppHandle) {
    tauri::async_runtime::spawn(async move {
        let built = zbus::connection::Builder::session()
            .and_then(|builder| builder.name(BUS_NAME))
            .and_then(|builder| builder.serve_at(OBJECT_PATH, Control { app }));
        let connection = match built {
            Ok(builder) => builder.build().await,
            Err(error) => Err(error),
        };
        match connection {
            Ok(connection) => {
                eprintln!("CONTROL serving {BUS_NAME}");
                // The connection serves requests until it is dropped; keep it for the life of the process.
                let _keep = Arc::new(connection);
                std::future::pending::<()>().await;
            }
            Err(error) => eprintln!("CONTROL could not serve {BUS_NAME}: {error}"),
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_served_methods_are_exactly_the_contract() {
        let interfaces = helix_desktop_api::parse_interfaces(include_str!(
            "../../../../protocols/dbus/org.helix.Shell.xml"
        ));
        let contract = &interfaces["org.helix.Shell1"];
        let mut served: Vec<&str> = METHODS.to_vec();
        let mut declared: Vec<&str> = contract.methods.iter().map(String::as_str).collect();
        served.sort_unstable();
        declared.sort_unstable();
        assert_eq!(served, declared);
        assert!(contract.signals.is_empty());
    }

    #[test]
    fn unknown_panel_ids_are_refused_not_ignored() {
        assert!(
            known("launcher").is_ok()
                && known("control-center").is_ok()
                && known("assistant").is_ok()
        );
        for bad in [
            "",
            "settings",
            "Launcher",
            "launcher ",
            "../x",
            "launcher\0",
        ] {
            assert!(known(bad).is_err(), "{bad:?}");
        }
    }
}
