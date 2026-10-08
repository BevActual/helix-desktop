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
    /// The person chose to approve even where the compositor cannot protect the dialog.
    allow_unprotected: bool,
}

/// Whether the person allows approvals the compositor cannot protect: `HELIX_ALLOW_UNPROTECTED_APPROVALS=1` (the shell's own
/// environment, which a program cannot change) or `allow_unprotected` in `~/.config/helix/approvals.json`. Off unless chosen.
fn unprotected_allowed() -> bool {
    if let Some(value) = std::env::var_os("HELIX_ALLOW_UNPROTECTED_APPROVALS") {
        return value == "1";
    }
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(std::path::PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| std::path::PathBuf::from(h).join(".config")));
    base.map(|b| b.join("helix/approvals.json"))
        .and_then(|path| std::fs::read_to_string(path).ok())
        .and_then(|text| serde_json::from_str::<Value>(&text).ok())
        .and_then(|v| v.get("allow_unprotected").and_then(Value::as_bool))
        .unwrap_or(false)
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
        let mut panel = panel;
        panel.require_protection();
        Self {
            panel,
            started,
            allow_unprotected: unprotected_allowed(),
        }
    }

    pub fn needs_protection_check(&self) -> bool {
        self.panel.needs_protection_check()
    }

    pub fn set_protection(&mut self, protected: bool) {
        let now = self.now_ms();
        self.panel.set_protection(protected, self.allow_unprotected, now);
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

    /// `operation` is the request the person was looking at when they clicked; anything else is not honoured.
    pub fn allow(&mut self, operation: u64) -> ConfirmerView {
        self.panel.allow(operation, self.now_ms());
        self.panel.view(self.now_ms())
    }

    pub fn deny(&mut self, operation: u64) -> ConfirmerView {
        self.panel.deny(operation, self.now_ms());
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

    pub fn set_tier(&mut self, operation: u64, name: &str, checked: bool) -> ConfirmerView {
        self.panel.set_tier(operation, name, checked);
        self.panel.view(self.now_ms())
    }

    pub fn present(&mut self) -> ConfirmerView {
        self.panel.present(self.now_ms());
        self.panel.view(self.now_ms())
    }

    pub fn open(&self) -> bool {
        self.panel.front().is_some()
    }
}

fn setup_overlay(window: &tauri::WebviewWindow) -> Result<(), String> {
    use gtk_layer_shell::Edge;
    let gtk_window = window.gtk_window().map_err(|e| e.to_string())?;
    // Full-output Overlay so layout boxes are screen-relative (click_id uses fractions of the
    // output). The card is centered in CSS; the surface itself is not a panel in the shell page.
    gtk_window.set_size_request(1, 1);
    gtk_window.init_layer_shell();
    gtk_window.set_namespace("helix-tauri-confirmer");
    gtk_window.set_layer(gtk_layer_shell::Layer::Overlay);
    for edge in [Edge::Top, Edge::Bottom, Edge::Left, Edge::Right] {
        gtk_window.set_anchor(edge, true);
    }
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
        .build()
        .map_err(|e| e.to_string())?;
    setup_overlay(&window)?;
    Ok(())
}

fn destroy_window(app: &AppHandle, keep_webkit: &BTreeSet<u32>) {
    if let Some(window) = app.get_webview_window(WINDOW) {
        // Snapshot before destroy: a delayed reap must not kill a confirmer opened later.
        let candidates = webkit_web_pids(std::process::id());
        let _ = window.destroy();
        eprintln!("CONFIRMER destroyed");
        reap_extra_webkits(keep_webkit.clone(), candidates);
    }
}

fn emit_view(app: &AppHandle, view: &ConfirmerView) {
    // Emit to the confirmer webview by label. App-wide emit can miss a window that just
    // appeared, and a capability mismatch then looks like a blank dialog.
    if let Some(window) = app.get_webview_window(WINDOW) {
        let _ = window.emit("confirmer-view", view);
    } else {
        let _ = app.emit("confirmer-view", view);
    }
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

/// Finds out whether the compositor treats the dialog as a trusted surface (kept on top, capture-proof, closed to synthetic input), then
/// tells the model. The compositor decides when the surface maps, so this asks until it appears; a compositor that never says yes (stock
/// niri, an executable not on its list) is "unprotected".
pub fn probe_protection(app: AppHandle, host: Arc<Mutex<ConfirmerHost>>) {
    std::thread::spawn(move || {
        let mut trusted = false;
        for _ in 0..25 {
            match crate::niri_query::layer_is_trusted("helix-tauri-confirmer") {
                Some(answer) => {
                    trusted = answer;
                    break;
                }
                None => std::thread::sleep(Duration::from_millis(100)),
            }
        }
        eprintln!("CONFIRMER protection={}", if trusted { "protected" } else { "unprotected" });
        let runner = app.clone();
        let _ = app.run_on_main_thread(move || {
            let keep = crate::confirmer_keep(&runner);
            let mut panel = host.lock().expect("confirmer");
            panel.set_protection(trusted);
            let view = panel.view();
            if view.open {
                // The window is already up; only its content changes (and it is destroyed below when the refusal emptied the queue).
                emit_view(&runner, &view);
            } else {
                sync_surface(&runner, &panel, &keep);
            }
            drop(panel);
            if view.open {
                schedule_arm(runner.clone(), host.clone());
            }
        });
    });
}

/// Wake the page once Allow becomes armed for the request currently showing.
pub fn schedule_arm(app: AppHandle, host: Arc<Mutex<ConfirmerHost>>) {
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(helix_shell_core::ARMING_MS));
        let runner = app.clone();
        let _ = app.run_on_main_thread(move || {
            let panel = host.lock().expect("confirmer");
            if panel.open() {
                eprintln!("CONFIRMER armed");
                emit_view(&runner, &panel.view());
            }
        });
    });
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
        // AgentActivity1 answers only the connection that registered as the desktop, so activity uses this very connection.
        let desktop = Arc::new(desktop);
        let _ = crate::REGISTERED.set(desktop.clone());
        crate::watch_activity(app.clone());
        keep_roles(app.clone(), desktop.clone());

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
                        let arm_host = host.clone();
                        let _ = app.run_on_main_thread(move || {
                            let keep = keep.lock().expect("webkit baseline").clone();
                            let already_open = runner.get_webview_window(WINDOW).is_some();
                            let mut panel = host.lock().expect("confirmer");
                            panel.requested(details);
                            let needs_arm = panel.open() && !panel.view().armed;
                            sync_surface(&runner, &panel, &keep);
                            drop(panel);
                            // New Overlay: arming starts in ready() once the page can paint.
                            // Already-visible Overlay (queued request): arm from here.
                            if already_open && needs_arm {
                                schedule_arm(runner.clone(), arm_host);
                            }
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
                        let arm_host = host.clone();
                        let _ = app.run_on_main_thread(move || {
                            let keep = keep.lock().expect("webkit baseline").clone();
                            let mut panel = host.lock().expect("confirmer");
                            panel.ended(operation);
                            let needs_arm = panel.open() && !panel.view().armed;
                            sync_surface(&runner, &panel, &keep);
                            drop(panel);
                            if needs_arm {
                                schedule_arm(runner.clone(), arm_host);
                            }
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

/// The desktop and confirmer roles belong to the service's current process: when it restarts they are gone, and a shell that does
/// not claim them again would silently stop showing approvals. Follows the name's owner (a signal, not a poll) and registers again.
fn keep_roles(app: AppHandle, desktop: Arc<Desktop>) {
    tauri::async_runtime::spawn(async move {
        let Ok(mut starts) = desktop.watch_service_starts().await else {
            return;
        };
        while starts.next().await.is_some() {
            eprintln!("CONFIRMER service restarted");
            for _ in 0..50 {
                if desktop.register_desktop().await.is_ok()
                    && desktop.confirmer().register().await.is_ok()
                {
                    eprintln!("CONFIRMER registered");
                    break;
                }
                tokio::time::sleep(Duration::from_millis(200)).await;
            }
            crate::refresh_activity(&app).await;
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

fn reap_extra_webkits(keep: BTreeSet<u32>, candidates: BTreeSet<u32>) {
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(750));
        let still_alive = webkit_web_pids(std::process::id());
        for pid in candidates {
            if keep.contains(&pid) || !still_alive.contains(&pid) {
                continue;
            }
            let result = unsafe { libc::kill(pid as i32, libc::SIGTERM) };
            eprintln!("WEBKIT reap {pid} result={result}");
        }
    });
}
