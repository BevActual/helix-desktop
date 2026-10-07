//! `org.helix.Shell1` (protocols/dbus/org.helix.Shell.xml): lets a key binding or another program open the shell's panels.
//! It only shows or hides UI, so it needs no grant. Every id is checked by the surface state machine, never trusted here.

use std::sync::Arc;

use tauri::AppHandle;
use zbus::zvariant::{OwnedValue, Value};

use crate::{close_panel, open_panel, shell_state};

pub const BUS_NAME: &str = "org.helix.Shell";
pub const OBJECT_PATH: &str = "/org/helix/Shell";
/// The methods this object serves. A test checks them against the XML contract.
#[cfg(test)]
pub const METHODS: &[&str] = &["OpenPanel", "TogglePanel", "ClosePanel", "GetStatus"];

struct Control {
    app: AppHandle,
}

fn current_panel(app: &AppHandle) -> Option<String> {
    shell_state(app)
        .lock()
        .expect("shell state")
        .surface
        .open_panel_id()
        .map(str::to_string)
}

/// Runs `work` on the main thread, where GTK must be touched.
fn on_main(
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
        let is_open = current_panel(&self.app).as_deref() == Some(id.as_str());
        on_main(&self.app, move |app| {
            if is_open {
                close_panel(app);
            } else {
                open_panel(app, &id);
            }
        })
    }

    async fn close_panel(&self) -> zbus::fdo::Result<()> {
        on_main(&self.app, close_panel)
    }

    async fn get_status(&self) -> std::collections::HashMap<String, OwnedValue> {
        let panel = current_panel(&self.app).unwrap_or_default();
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
