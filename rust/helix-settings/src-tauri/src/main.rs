//! Helix Settings. A separate Tauri application with one window and a page per area; this file is the thin layer between the pages and
//! `helix-shell-core` / `helix-desktop-api`. It holds the Agent1 *manager* role (and nothing else of Agent1): it can read who has which
//! tiers and the audit, change tiers and revoke, but it is not an agent, cannot approve anything, and never talks to the model. What a
//! change may be is decided by the pure models in `helix-shell-core`; the page only sends names those models listed.

use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

use helix_desktop_api::Desktop;
use helix_shell_core::grants::{Grants, GrantsView, Request, RequestError};
use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindowBuilder};

const WINDOW: &str = "settings";

struct State {
    grants: Grants,
    status: &'static str,
}

type Shared = Arc<Mutex<State>>;

/// The connection that registered as manager: the service knows the manager by this connection, so every change goes through it.
static DESKTOP: OnceLock<Arc<Desktop>> = OnceLock::new();

#[derive(Serialize, Clone)]
struct AgentsView {
    /// `connected`, `denied` (this copy is not the installed Settings app) or `unavailable` (no desktop service).
    status: &'static str,
    #[serde(flatten)]
    view: GrantsView,
}

fn shared(app: &AppHandle) -> Shared {
    app.state::<Shared>().inner().clone()
}

fn agents_view_of(state: &State) -> AgentsView {
    AgentsView {
        status: state.status,
        view: state.grants.view(),
    }
}

fn publish(app: &AppHandle) {
    let view = agents_view_of(&shared(app).lock().expect("state"));
    eprintln!(
        "SETTINGS agents status={} clients={}",
        view.status,
        view.view.clients.len()
    );
    let _ = app.emit("agents-view", view);
}

/// Reads grants and the audit again and tells the page.
async fn refresh(app: &AppHandle) {
    let Some(desktop) = DESKTOP.get() else { return };
    let manager = desktop.manager();
    let grants = manager.grants().await;
    let audit = manager.audit(0, 400).await;
    {
        let shared = shared(app);
        let mut state = shared.lock().expect("state");
        match grants {
            Ok(grants) => state.grants.set_grants(&grants),
            Err(error) => eprintln!("SETTINGS grants: {error}"),
        }
        if let Ok(audit) = audit {
            state.grants.set_audit(&audit);
        }
    }
    publish(app);
}

fn set_status(app: &AppHandle, status: &'static str) {
    let changed = {
        let shared = shared(app);
        let mut state = shared.lock().expect("state");
        std::mem::replace(&mut state.status, status) != status
    };
    if changed {
        publish(app);
    }
}

/// Keeps the manager role: registers (retrying while the service is absent or refuses), reads, and follows `Changed`.
fn connect(app: AppHandle) {
    tauri::async_runtime::spawn(async move {
        loop {
            match Desktop::session().await {
                Ok(desktop) => {
                    let desktop = Arc::new(desktop);
                    match desktop.manager().register().await {
                        Ok(()) => {
                            let _ = DESKTOP.set(desktop.clone());
                            set_status(&app, "connected");
                            refresh(&app).await;
                            let watcher = app.clone();
                            let _ = desktop
                                .manager()
                                .watch(move || {
                                    let app = watcher.clone();
                                    tauri::async_runtime::spawn(async move { refresh(&app).await });
                                })
                                .await;
                        }
                        Err(error) if error.to_string().contains("Only Helix Settings") => {
                            set_status(&app, "denied")
                        }
                        Err(_) => set_status(&app, "unavailable"),
                    }
                }
                Err(_) => set_status(&app, "unavailable"),
            }
            tokio::time::sleep(Duration::from_secs(3)).await;
        }
    });
}

#[tauri::command]
fn agents_view(app: AppHandle) -> AgentsView {
    agents_view_of(&shared(&app).lock().expect("state"))
}

async fn send(app: &AppHandle, request: Result<Request, RequestError>) -> Result<(), String> {
    let request = request.map_err(|error| format!("{error:?}"))?;
    let desktop = DESKTOP
        .get()
        .ok_or("not connected to the desktop service")?;
    let manager = desktop.manager();
    match &request {
        Request::SetGrant { client, tiers } => manager.set_grant(client, tiers).await,
        Request::Revoke { client } => manager.revoke(client).await,
    }
    .map_err(|error| error.to_string())?;
    eprintln!(
        "SETTINGS agents {}",
        match &request {
            Request::SetGrant { .. } => "set",
            Request::Revoke { .. } => "revoke",
        }
    );
    refresh(app).await;
    Ok(())
}

#[tauri::command]
async fn agents_set_tier(
    app: AppHandle,
    client: String,
    tier: String,
    on: bool,
) -> Result<(), String> {
    let request = shared(&app)
        .lock()
        .expect("state")
        .grants
        .set_tier(&client, &tier, on);
    send(&app, request).await
}

#[tauri::command]
async fn agents_revoke(app: AppHandle, client: String) -> Result<(), String> {
    let request = shared(&app).lock().expect("state").grants.revoke(&client);
    send(&app, request).await
}

#[tauri::command]
async fn agents_refresh(app: AppHandle) {
    refresh(&app).await;
}

/// Whether the page should report where its controls are (a test asked for it).
#[tauri::command]
fn layout_reporting() -> bool {
    std::env::var_os("HELIX_UI_LAYOUT_LOG").is_some()
}

#[tauri::command]
fn report_layout(report: serde_json::Value) {
    let Some(path) = std::env::var_os("HELIX_UI_LAYOUT_LOG") else {
        return;
    };
    let path = std::path::PathBuf::from(path);
    let text = report.to_string();
    if text.len() > 64 * 1024 {
        return;
    }
    let temporary = path.with_extension("tmp");
    if std::fs::write(&temporary, &text).is_ok() {
        let _ = std::fs::rename(&temporary, &path);
    }
}

fn main() {
    // The web view's JIT costs memory this app does not need (same choice as the shell).
    // The window announces this application id, which is what the desktop file org.helix.Settings.desktop (name and icon) is matched by.
    gtk::glib::set_prgname(Some("org.helix.Settings"));
    if std::env::var_os("JSC_useJIT").is_none() {
        std::env::set_var("JSC_useJIT", "false");
    }
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            agents_view,
            agents_set_tier,
            agents_revoke,
            agents_refresh,
            layout_reporting,
            report_layout
        ])
        .setup(|app| {
            app.manage::<Shared>(Arc::new(Mutex::new(State {
                grants: Grants::default(),
                status: "unavailable",
            })));
            WebviewWindowBuilder::new(app, WINDOW, WebviewUrl::App("index.html".into()))
                .title("Settings")
                .decorations(false)
                .transparent(true)
                .inner_size(1040.0, 720.0)
                .min_inner_size(800.0, 560.0)
                .build()?;
            connect(app.handle().clone());
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running Helix Settings");
}
