//! Helix Software. Thin host: Flatpak (and later PackageKit / fwupd) feeds the pure
//! `helix_shell_core::software::Store`; the page draws sanitized views and asks for confirmed mutations.
//! Nested tests set `HELIX_SOFTWARE_FAKE` to a JSON catalog and never run real installs.

use std::path::PathBuf;
use std::process::Command;
use std::sync::{Arc, Mutex};

use helix_shell_core::software::{valid_id, Kind, Mutation, Package, PackageView, Store};
use serde::Serialize;
use tauri::{AppHandle, Manager, WebviewUrl, WebviewWindowBuilder};

const WINDOW: &str = "software";

struct State {
    store: Store,
    fake: bool,
    pending: Option<(String, Mutation)>,
}

type Shared = Arc<Mutex<State>>;

fn shared(app: &AppHandle) -> Shared {
    app.state::<Shared>().inner().clone()
}

fn startup_app() -> Option<String> {
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        if let Some(id) = arg.strip_prefix("--app=") {
            return valid_id(id).then(|| id.to_string());
        }
        if arg == "--app" {
            let id = args.next()?;
            return valid_id(&id).then_some(id);
        }
    }
    None
}

fn startup_page() -> Option<String> {
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        if arg == "--page" {
            return args.next().filter(|p| matches!(p.as_str(), "explore" | "library" | "updates" | "sources"));
        }
        if let Some(page) = arg.strip_prefix("--page=") {
            if matches!(page, "explore" | "library" | "updates" | "sources") {
                return Some(page.to_string());
            }
        }
    }
    None
}

fn load_fake(path: &PathBuf) -> Vec<Package> {
    let Ok(text) = std::fs::read_to_string(path) else {
        return Vec::new();
    };
    let Ok(value) = serde_json::from_str::<serde_json::Value>(&text) else {
        return Vec::new();
    };
    value
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|row| {
            Some(Package {
                id: row.get("id")?.as_str()?.to_string(),
                title: row.get("title").and_then(|v| v.as_str()).unwrap_or("").to_string(),
                summary: row.get("summary").and_then(|v| v.as_str()).unwrap_or("").to_string(),
                kind: match row.get("kind").and_then(|v| v.as_str()).unwrap_or("flatpak") {
                    "rpm" => Kind::Rpm,
                    "firmware" => Kind::Firmware,
                    _ => Kind::Flatpak,
                },
                installed: row.get("installed").and_then(|v| v.as_bool()).unwrap_or(false),
                update: row.get("update").and_then(|v| v.as_bool()).unwrap_or(false),
                remote: row.get("remote").and_then(|v| v.as_str()).unwrap_or("").to_string(),
            })
        })
        .collect()
}

/// Flatpak remote-ls shaped lines: `application\tname\tsummary` (best-effort; empty when unavailable).
fn load_flatpak() -> Vec<Package> {
    let output = Command::new("flatpak")
        .args(["remote-ls", "--app", "--columns=application,name,summary,origin"])
        .output();
    let Ok(output) = output else {
        return Vec::new();
    };
    if !output.status.success() {
        return Vec::new();
    }
    let text = String::from_utf8_lossy(&output.stdout);
    let installed = {
        let list = Command::new("flatpak")
            .args(["list", "--app", "--columns=application"])
            .output()
            .ok()
            .filter(|o| o.status.success())
            .map(|o| String::from_utf8_lossy(&o.stdout).to_string())
            .unwrap_or_default();
        list.lines().map(str::trim).filter(|l| !l.is_empty()).map(str::to_string).collect::<std::collections::HashSet<_>>()
    };
    text.lines()
        .filter_map(|line| {
            let mut parts = line.split('\t');
            let id = parts.next()?.trim().to_string();
            if !valid_id(&id) {
                return None;
            }
            let title = parts.next().unwrap_or(&id).to_string();
            let summary = parts.next().unwrap_or("").to_string();
            let remote = parts.next().unwrap_or("").to_string();
            Some(Package {
                installed: installed.contains(&id),
                update: false,
                id,
                title,
                summary,
                kind: Kind::Flatpak,
                remote,
            })
        })
        .take(500)
        .collect()
}

fn refresh_catalog(state: &mut State) {
    if state.fake {
        return;
    }
    let mut packages = load_flatpak();
    // PackageKit / firmware: best-effort stubs for Sources / Updates until fuller backends land.
    if let Ok(output) = Command::new("fwupdmgr").args(["get-updates", "--json"]).output() {
        if output.status.success() {
            if let Ok(value) = serde_json::from_slice::<serde_json::Value>(&output.stdout) {
                if let Some(devices) = value.get("Devices").and_then(|d| d.as_array()) {
                    for (i, device) in devices.iter().enumerate().take(40) {
                        let name = device.get("Name").and_then(|v| v.as_str()).unwrap_or("Firmware");
                        let id = format!("fwupd-{i}");
                        packages.push(Package {
                            id,
                            title: name.to_string(),
                            summary: "Firmware update".into(),
                            kind: Kind::Firmware,
                            installed: true,
                            update: true,
                            remote: "fwupd".into(),
                        });
                    }
                }
            }
        }
    }
    state.store.set(packages);
}

#[derive(Serialize)]
struct PageView {
    explore: Vec<PackageView>,
    library: Vec<PackageView>,
    updates: Vec<PackageView>,
    detail: Option<PackageView>,
    pending: Option<String>,
    sources: Vec<String>,
}

#[tauri::command]
fn software_view(app: AppHandle, page: String, query: String, kind: String) -> PageView {
    let kind = match kind.as_str() {
        "rpm" => Kind::Rpm,
        "firmware" => Kind::Firmware,
        _ => Kind::Flatpak,
    };
    let shared = shared(&app);
    let state = shared.lock().expect("state");
    let pending = state.pending.as_ref().map(|(id, m)| format!("{id}:{:?}", m));
    PageView {
        explore: if page == "explore" { state.store.explore(kind, &query) } else { Vec::new() },
        library: if page == "library" { state.store.library(kind) } else { Vec::new() },
        updates: if page == "updates" { state.store.updates() } else { Vec::new() },
        detail: None,
        pending,
        sources: if page == "sources" {
            vec!["flathub".into(), "fedora".into(), "fwupd".into()]
        } else {
            Vec::new()
        },
    }
}

#[tauri::command]
fn software_detail(app: AppHandle, id: String) -> Option<PackageView> {
    let shared = shared(&app);
    let state = shared.lock().expect("state");
    state.store.detail(&id)
}

#[tauri::command]
fn software_request(app: AppHandle, id: String, action: String) -> Result<PackageView, String> {
    let mutation = match action.as_str() {
        "install" => Mutation::Install,
        "remove" => Mutation::Remove,
        "update" => Mutation::Update,
        _ => return Err("unknown action".into()),
    };
    let shared = shared(&app);
    let mut state = shared.lock().expect("state");
    let view = state.store.prepare(&id, mutation).map_err(|e| format!("{e:?}"))?;
    state.pending = Some((id, mutation));
    Ok(view)
}

#[tauri::command]
fn software_confirm(app: AppHandle, accept: bool) -> Result<(), String> {
    let shared = shared(&app);
    let mut state = shared.lock().expect("state");
    let Some((id, mutation)) = state.pending.take() else {
        return Ok(());
    };
    if !accept {
        return Ok(());
    }
    if state.fake {
        state.store.apply(&id, mutation).map_err(|e| format!("{e:?}"))?;
        eprintln!("SOFTWARE fake {:?} {id}", mutation);
        return Ok(());
    }
    let status = match mutation {
        Mutation::Install => Command::new("flatpak").args(["install", "-y", &id]).status(),
        Mutation::Remove => Command::new("flatpak").args(["uninstall", "-y", &id]).status(),
        Mutation::Update => Command::new("flatpak").args(["update", "-y", &id]).status(),
    };
    match status {
        Ok(code) if code.success() => {
            state.store.apply(&id, mutation).map_err(|e| format!("{e:?}"))?;
            Ok(())
        }
        Ok(_) => Err("package command failed".into()),
        Err(error) => Err(error.to_string()),
    }
}

#[tauri::command]
fn software_refresh(app: AppHandle) {
    let shared = shared(&app);
    let mut state = shared.lock().expect("state");
    if let Some(path) = std::env::var_os("HELIX_SOFTWARE_FAKE").map(PathBuf::from) {
        state.store.set(load_fake(&path));
        state.fake = true;
    } else {
        refresh_catalog(&mut state);
    }
}

#[tauri::command]
fn layout_reporting() -> bool {
    std::env::var_os("HELIX_UI_LAYOUT_LOG").is_some()
}

#[tauri::command]
fn report_layout(report: serde_json::Value) {
    let Some(path) = std::env::var_os("HELIX_UI_LAYOUT_LOG") else {
        return;
    };
    let path = PathBuf::from(path);
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
    gtk::glib::set_prgname(Some("org.helix.Software"));
    if std::env::var_os("JSC_useJIT").is_none() {
        std::env::set_var("JSC_useJIT", "false");
    }
    let deep = startup_app();
    let page = startup_page();
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            software_view,
            software_detail,
            software_request,
            software_confirm,
            software_refresh,
            layout_reporting,
            report_layout
        ])
        .setup(move |app| {
            let mut store = Store::default();
            let mut fake = false;
            if let Some(path) = std::env::var_os("HELIX_SOFTWARE_FAKE").map(PathBuf::from) {
                store.set(load_fake(&path));
                fake = true;
                eprintln!("SOFTWARE fake catalog {}", path.display());
            } else {
                let mut state = State { store: Store::default(), fake: false, pending: None };
                refresh_catalog(&mut state);
                store = state.store;
            }
            app.manage::<Shared>(Arc::new(Mutex::new(State { store, fake, pending: None })));
            let url = if let Some(id) = &deep {
                format!("index.html?app={id}")
            } else if let Some(page) = &page {
                format!("index.html?page={page}")
            } else {
                "index.html".into()
            };
            WebviewWindowBuilder::new(app, WINDOW, WebviewUrl::App(url.into()))
                .title("Helix Software")
                .decorations(false)
                .transparent(true)
                .inner_size(1040.0, 720.0)
                .min_inner_size(800.0, 560.0)
                .build()?;
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running Helix Software");
}
