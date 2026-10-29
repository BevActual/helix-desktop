//! Run launcher desktop actions (`helix:desktop:*`).

use tauri::AppHandle;

use crate::{open_panel, shell_state};

pub fn run(app: &AppHandle, id: &str) -> Result<(), String> {
    match id {
        "helix:desktop:agent-desk" => crate::dock::agent_desk_focus(),
        "helix:desktop:notifications" => {
            open_panel(app, "notifications");
            Ok(())
        }
        "helix:desktop:control-center" => {
            open_panel(app, "control-center");
            Ok(())
        }
        "helix:desktop:launcher" => {
            open_panel(app, "launcher");
            Ok(())
        }
        "helix:desktop:writing" => {
            open_panel(app, "writing");
            Ok(())
        }
        "helix:desktop:assistant" => {
            open_panel(app, "assistant");
            Ok(())
        }
        "helix:desktop:agents" => {
            open_panel(app, "agents");
            Ok(())
        }
        "helix:desktop:settings" => spawn_settings(None),
        "helix:desktop:privacy-agents" => spawn_settings(Some("agents")),
        "helix:desktop:mobius" => spawn_bin("helix-mobius"),
        "helix:desktop:software" => spawn_bin("helix-software"),
        "helix:desktop:system-monitor" => spawn_bin("helix-system-monitor"),
        "helix:desktop:lock-screen" => {
            crate::insert::lock_session();
            Ok(())
        }
        "helix:desktop:night-light" => {
            let app = app.clone();
            tauri::async_runtime::spawn(async move {
                crate::toggle_night_light(&app).await;
            });
            Ok(())
        }
        "helix:desktop:dark-theme" => {
            let app = app.clone();
            tauri::async_runtime::spawn(async move {
                crate::toggle_dark_theme(&app).await;
            });
            Ok(())
        }
        "helix:desktop:dnd" => {
            let app = app.clone();
            tauri::async_runtime::spawn(async move {
                crate::notify::toggle_dnd(&app).await;
            });
            Ok(())
        }
        "helix:desktop:workspace-next" => {
            crate::dock::niri_action(&serde_json::json!({"FocusWorkspaceDown": {}}))
        }
        "helix:desktop:workspace-prev" => {
            crate::dock::niri_action(&serde_json::json!({"FocusWorkspaceUp": {}}))
        }
        "helix:desktop:bar-show" => crate::bar::set_visible(app, true),
        "helix:desktop:bar-hide" => crate::bar::set_visible(app, false),
        "helix:desktop:bar-toggle" => crate::bar::toggle(app),
        "helix:desktop:close-window" => {
            let focused = shell_state(app)
                .lock()
                .expect("shell state")
                .niri
                .focused_window()
                .map(|w| w.id);
            let Some(id) = focused else {
                return Err("no focused window".into());
            };
            crate::dock::niri_action(&serde_json::json!({"CloseWindow": {"id": id}}))
        }
        _ => Err("unknown desktop action".into()),
    }
}

fn spawn_bin(name: &str) -> Result<(), String> {
    let mut command = std::process::Command::new(name);
    crate::apps::scrub_shell_env(&mut command);
    command.spawn().map_err(|e| e.to_string())?;
    Ok(())
}

pub(crate) fn spawn_settings(page: Option<&str>) -> Result<(), String> {
    let mut command = std::process::Command::new("helix-settings");
    crate::apps::scrub_shell_env(&mut command);
    if let Some(page) = page {
        command.arg("--page").arg(page);
    }
    command.spawn().map_err(|e| e.to_string())?;
    Ok(())
}
