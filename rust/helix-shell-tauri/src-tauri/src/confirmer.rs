//! On-demand Agent1 confirmer Overlay. Created when a request is showing, destroyed when the queue
//! is empty. Exclusive keyboard; not dismissed by an outside click. Text reaches the page only as
//! a ConfirmerView (already sanitized). RegisterDesktop and RegisterConfirmer share one D-Bus
//! connection: confirmer identity is the connection.

use std::collections::BTreeSet;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use gtk::prelude::*;
use gtk_layer_shell::LayerShell;
use helix_desktop_api::Desktop;
use helix_shell_core::{Confirmer, ConfirmerView};
use serde_json::Value;
use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindowBuilder};
use tokio::sync::mpsc;

pub const WINDOW: &str = "confirmer";

pub enum Answer {
    Confirm {
        operation: u64,
        approved: bool,
        tiers: Vec<String>,
    },
}

pub struct ConfirmerHost {
    panel: Confirmer,
    started: Instant,
}

impl ConfirmerHost {
    pub fn new(answers: mpsc::UnboundedSender<Answer>, started: Instant) -> Self {
        let panel = Confirmer::new(Box::new(move |operation, approved, tiers| {
            let _ = answers.send(Answer::Confirm {
                operation,
                approved,
                tiers,
            });
        }));
        Self { panel, started }
    }

    fn now_ms(&self) -> u64 {
        u64::try_from(self.started.elapsed().as_millis()).unwrap_or(u64::MAX)
    }

    pub fn view(&self) -> ConfirmerView {
        self.panel.view(self.now_ms())
    }

    pub fn requested(&mut self, details: Value) {
        self.panel.requested(&details, self.now_ms());
    }

    pub fn ended(&mut self, operation: u64) {
        self.panel.ended(operation, self.now_ms());
    }

    pub fn allow(&mut self) -> ConfirmerView {
        self.panel.allow(self.now_ms());
        self.panel.view(self.now_ms())
    }

    pub fn deny(&mut self) -> ConfirmerView {
        self.panel.deny(self.now_ms());
        self.panel.view(self.now_ms())
    }

    pub fn enter(&mut self) -> ConfirmerView {
        self.panel.enter(self.now_ms());
        self.panel.view(self.now_ms())
    }

    pub fn escape(&mut self) -> ConfirmerView {
        self.panel.close(self.now_ms());
        self.panel.view(self.now_ms())
    }

    pub fn set_tier(&mut self, name: &str, checked: bool) -> ConfirmerView {
        self.panel.set_tier(name, checked);
        self.panel.view(self.now_ms())
    }

    pub fn open(&self) -> bool {
        self.panel.front().is_some()
    }
}

fn setup_overlay(window: &tauri::WebviewWindow) -> Result<(), String> {
    let gtk_window = window.gtk_window().map_err(|e| e.to_string())?;
    gtk_window.set_size_request(520, 320);
    gtk_window.init_layer_shell();
    gtk_window.set_namespace("helix-tauri-confirmer");
    gtk_window.set_layer(gtk_layer_shell::Layer::Overlay);
    gtk_window.set_keyboard_mode(gtk_layer_shell::KeyboardMode::Exclusive);
    gtk_window.set_exclusive_zone(0);
    window.show().map_err(|e| e.to_string())?;
    Ok(())
}

fn ensure_window(app: &AppHandle) -> Result<(), String> {
    if app.get_webview_window(WINDOW).is_some() {
        return Ok(());
    }
    let window = WebviewWindowBuilder::new(app, WINDOW, WebviewUrl::App("confirmer.html".into()))
        .decorations(false)
        .transparent(true)
        .visible(false)
        .inner_size(520.0, 320.0)
        .build()
        .map_err(|e| e.to_string())?;
    setup_overlay(&window)?;
    Ok(())
}

fn destroy_window(app: &AppHandle, keep_webkit: &BTreeSet<u32>) {
    if let Some(window) = app.get_webview_window(WINDOW) {
        let _ = window.destroy();
        eprintln!("CONFIRMER destroyed");
        reap_extra_webkits(keep_webkit.clone());
    }
}

fn emit_view(app: &AppHandle, view: &ConfirmerView) {
    let _ = app.emit("confirmer-view", view);
}

pub fn sync_surface(app: &AppHandle, host: &ConfirmerHost, keep_webkit: &BTreeSet<u32>) {
    if host.open() {
        if let Err(error) = ensure_window(app) {
            eprintln!("confirmer window: {error}");
            return;
        }
        emit_view(app, &host.view());
        eprintln!("CONFIRMER open");
    } else {
        destroy_window(app, keep_webkit);
    }
}

/// One Desktop connection: RegisterDesktop, RegisterConfirmer, watch, and Confirm all use it.
pub fn start(
    app: AppHandle,
    host: Arc<Mutex<ConfirmerHost>>,
    keep_webkit: Arc<Mutex<BTreeSet<u32>>>,
    mut answers: mpsc::UnboundedReceiver<Answer>,
) {
    tauri::async_runtime::spawn(async move {
        // Wait until the nested helix-service owns the name. Connecting early activates the
        // installed org.helix.Desktop on this private bus with the dbus-daemon's HOME (the real one).
        for _ in 0..50 {
            if name_has_owner().await {
                break;
            }
            tokio::time::sleep(Duration::from_millis(200)).await;
        }
        let mut desktop: Option<Desktop> = None;
        for _ in 0..50 {
            if !name_has_owner().await {
                tokio::time::sleep(Duration::from_millis(200)).await;
                continue;
            }
            if let Ok(session) = Desktop::session().await {
                if session.register_desktop().await.is_err() {
                    tokio::time::sleep(Duration::from_millis(200)).await;
                    continue;
                }
                let confirmer = session.confirmer();
                if confirmer.register().await.is_ok() {
                    eprintln!("CONFIRMER registered");
                    desktop = Some(session);
                    break;
                }
            }
            tokio::time::sleep(Duration::from_millis(200)).await;
        }
        let Some(desktop) = desktop else {
            eprintln!("CONFIRMER registration failed");
            return;
        };
        let confirmer = desktop.confirmer();

        {
            let app = app.clone();
            let host = host.clone();
            let keep = keep_webkit.clone();
            let watcher = desktop.confirmer();
            tauri::async_runtime::spawn(async move {
                let _ = watcher
                    .watch_requested(move |details| {
                        let host = host.clone();
                        let runner = app.clone();
                        let keep = keep.clone();
                        let _ = app.run_on_main_thread(move || {
                            let keep = keep.lock().expect("webkit baseline").clone();
                            let mut panel = host.lock().expect("confirmer");
                            panel.requested(details);
                            sync_surface(&runner, &panel, &keep);
                        });
                    })
                    .await;
            });
        }
        {
            let app = app.clone();
            let host = host.clone();
            let keep = keep_webkit.clone();
            let watcher = desktop.confirmer();
            tauri::async_runtime::spawn(async move {
                let _ = watcher
                    .watch_ended(move |operation| {
                        let host = host.clone();
                        let runner = app.clone();
                        let keep = keep.clone();
                        let _ = app.run_on_main_thread(move || {
                            let keep = keep.lock().expect("webkit baseline").clone();
                            let mut panel = host.lock().expect("confirmer");
                            panel.ended(operation);
                            sync_surface(&runner, &panel, &keep);
                        });
                    })
                    .await;
            });
        }

        while let Some(Answer::Confirm {
            operation,
            approved,
            tiers,
        }) = answers.recv().await
        {
            if let Err(error) = confirmer.confirm(operation, approved, &tiers).await {
                eprintln!("CONFIRMER confirm failed: {error}");
            }
        }
    });
}

async fn name_has_owner() -> bool {
    let Ok(connection) = zbus::Connection::session().await else {
        return false;
    };
    let Ok(proxy) = zbus::Proxy::new(
        &connection,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
    )
    .await
    else {
        return false;
    };
    proxy
        .call("NameHasOwner", &("org.helix.Desktop",))
        .await
        .unwrap_or(false)
}

pub fn snapshot_webkit_web_pids() -> BTreeSet<u32> {
    webkit_web_pids(std::process::id())
}

fn webkit_web_pids(root: u32) -> BTreeSet<u32> {
    let mut ppid = std::collections::BTreeMap::<u32, u32>::new();
    let Ok(entries) = std::fs::read_dir("/proc") else {
        return BTreeSet::new();
    };
    for entry in entries.flatten() {
        let Ok(pid) = entry.file_name().to_string_lossy().parse::<u32>() else {
            continue;
        };
        let Ok(stat) = std::fs::read_to_string(format!("/proc/{pid}/stat")) else {
            continue;
        };
        let Some((_, rest)) = stat.rsplit_once(')') else {
            continue;
        };
        let mut parts = rest.split_whitespace();
        let _state = parts.next();
        let Some(parent) = parts.next().and_then(|value| value.parse().ok()) else {
            continue;
        };
        ppid.insert(pid, parent);
    }
    let mut found = vec![root];
    let mut frontier = vec![root];
    while !frontier.is_empty() {
        let mut next = Vec::new();
        for (&pid, &parent) in &ppid {
            if frontier.contains(&parent) && !found.contains(&pid) {
                found.push(pid);
                next.push(pid);
            }
        }
        frontier = next;
    }
    let mut pids = BTreeSet::new();
    for pid in found {
        let Ok(name) = std::fs::read_to_string(format!("/proc/{pid}/comm")) else {
            continue;
        };
        if name.trim().starts_with("WebKitWeb") {
            pids.insert(pid);
        }
    }
    pids
}

fn reap_extra_webkits(keep: BTreeSet<u32>) {
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(750));
        for pid in webkit_web_pids(std::process::id()) {
            if keep.contains(&pid) {
                continue;
            }
            let result = unsafe { libc::kill(pid as i32, libc::SIGTERM) };
            eprintln!("WEBKIT reap {pid} result={result}");
        }
    });
}
