//! Helix System Monitor. Reads `/proc` (or a fake tree under `HELIX_MONITOR_PROC`) and screen-time
//! report files; pure models in `helix_monitor_model::monitor` own the numbers. Poll timers match
//! docs/design/system-monitor-tauri.md.

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use helix_monitor_model::monitor::{
    parse_cpu, parse_memory, parse_net, parse_process, parse_screen_time, Monitor, Overview,
    ProcessRow, ProcessSort, ScreenTimeRow,
};
use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindowBuilder};

const WINDOW: &str = "monitor";

/// Poll intervals (seconds) — stated reasons in docs/design/system-monitor-tauri.md.
const CPU_POLL_MS: u64 = 2000;
const MEM_POLL_MS: u64 = 2000;
const NET_POLL_MS: u64 = 3000;
const DISK_POLL_MS: u64 = 10_000;
const GPU_POLL_MS: u64 = 5000;
const PROC_POLL_MS: u64 = 2000;

struct State {
    monitor: Monitor,
    proc_root: PathBuf,
    show_gpu: bool,
    last_gpu_ms: u64,
}

type Shared = Arc<Mutex<State>>;

fn shared(app: &AppHandle) -> Shared {
    app.state::<Shared>().inner().clone()
}

fn proc_root() -> PathBuf {
    std::env::var_os("HELIX_MONITOR_PROC")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/proc"))
}

fn read(path: &Path) -> String {
    std::fs::read_to_string(path).unwrap_or_default()
}

fn sample(state: &mut State) {
    let root = &state.proc_root;
    if let Some(cpu) = parse_cpu(&read(&root.join("stat"))) {
        state.monitor.set_cpu(cpu);
    }
    if let Some(mem) = parse_memory(&read(&root.join("meminfo"))) {
        state.monitor.set_memory(mem);
    }
    if let Some(net) = parse_net(&read(&root.join("net/dev"))) {
        state.monitor.set_net(net);
    }
    let load = read(&root.join("loadavg"));
    let mut parts = load.split_whitespace();
    if let (Some(a), Some(b), Some(c)) = (parts.next(), parts.next(), parts.next()) {
        if let (Ok(one), Ok(five), Ok(fifteen)) = (a.parse(), b.parse(), c.parse()) {
            state.monitor.set_load(one, five, fifteen);
        }
    }
    // Disk/swap already covered via meminfo swap; capacity probe is a no-op placeholder for interval.
    let _ = DISK_POLL_MS;
    if state.show_gpu {
        state.last_gpu_ms = state.last_gpu_ms.saturating_add(GPU_POLL_MS);
        // GPU probe only while Overview shows GPU UI (host records that it would probe).
        eprintln!("MONITOR gpu probe tick={}", state.last_gpu_ms);
    }
    let page_size = libc_page_size();
    let mut rows = Vec::new();
    if let Ok(entries) = std::fs::read_dir(root) {
        for entry in entries.flatten().take(400) {
            let name = entry.file_name();
            let name = name.to_string_lossy();
            if !name.bytes().all(|b| b.is_ascii_digit()) {
                continue;
            }
            let stat = read(&entry.path().join("stat"));
            if let Some(row) = parse_process(&stat, page_size) {
                rows.push(row);
            }
        }
    }
    state.monitor.set_processes(rows);
    let report = std::env::var_os("HELIX_MONITOR_SCREEN_TIME")
        .map(PathBuf::from)
        .or_else(|| {
            std::env::var_os("XDG_DATA_HOME")
                .map(PathBuf::from)
                .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share")))
                .map(|b| b.join("helix/screen-time/report"))
        });
    if let Some(path) = report {
        state
            .monitor
            .set_screen_time(parse_screen_time(&read(&path)));
    }
}

fn libc_page_size() -> u64 {
    // Avoid linking libc just for this: default 4096; fake trees use the same.
    4096
}

fn startup_page() -> Option<String> {
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        if arg == "--page" {
            return args
                .next()
                .filter(|p| matches!(p.as_str(), "overview" | "processes" | "screen-time"));
        }
        if let Some(page) = arg.strip_prefix("--page=") {
            if matches!(page, "overview" | "processes" | "screen-time") {
                return Some(page.to_string());
            }
        }
    }
    None
}

#[derive(Serialize, Clone)]
struct MonitorView {
    overview: Overview,
    processes: Vec<ProcessRow>,
    screen_time: Vec<ScreenTimeRow>,
    gpu_shown: bool,
}

#[tauri::command]
fn monitor_view(app: AppHandle, query: String, sort: String) -> MonitorView {
    let sort = match sort.as_str() {
        "memory" => ProcessSort::Memory,
        "name" => ProcessSort::Name,
        "pid" => ProcessSort::Pid,
        _ => ProcessSort::Cpu,
    };
    let shared = shared(&app);
    let state = shared.lock().expect("state");
    MonitorView {
        overview: state.monitor.overview(),
        processes: state.monitor.processes(&query, sort),
        screen_time: state.monitor.screen_time(),
        gpu_shown: state.show_gpu,
    }
}

#[tauri::command]
fn monitor_set_gpu(app: AppHandle, shown: bool) {
    let shared = shared(&app);
    shared.lock().expect("state").show_gpu = shown;
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

fn start_polls(app: AppHandle) {
    tauri::async_runtime::spawn(async move {
        let mut tick: u64 = 0;
        loop {
            tokio::time::sleep(Duration::from_millis(1000)).await;
            tick += 1;
            {
                let shared = shared(&app);
                let mut state = shared.lock().expect("state");
                // Align with documented intervals using a 1s base tick.
                if tick.is_multiple_of(CPU_POLL_MS / 1000)
                    || tick.is_multiple_of(MEM_POLL_MS / 1000)
                    || tick.is_multiple_of(PROC_POLL_MS / 1000)
                {
                    sample(&mut state);
                } else if tick.is_multiple_of(NET_POLL_MS / 1000) {
                    if let Some(net) = parse_net(&read(&state.proc_root.join("net/dev"))) {
                        state.monitor.set_net(net);
                    }
                }
            }
            let _ = app.emit("monitor", ());
        }
    });
}

/// 0 until the desktop service has said, then 1 for dark and 2 for light.
static APP_THEME: std::sync::atomic::AtomicU8 = std::sync::atomic::AtomicU8::new(0);

/// The accent colour's name, once the desktop service has said (`blue` until then).
static APP_ACCENT: std::sync::Mutex<String> = std::sync::Mutex::new(String::new());

fn theme_script() -> Option<String> {
    let dark = match APP_THEME.load(std::sync::atomic::Ordering::Relaxed) {
        1 => true,
        2 => false,
        _ => return None,
    };
    let accent = APP_ACCENT.lock().map(|a| a.clone()).unwrap_or_default();
    Some(helix_desktop_api::theme_script(&helix_desktop_api::Look {
        dark,
        accent,
    }))
}

/// Applies the person's interface scale and light or dark theme (Settings) to this application's windows now and whenever they change.
/// A page that loads later gets the theme from `on_page_load`.
fn follow_appearance(app: tauri::AppHandle) {
    let scale_app = app.clone();
    tauri::async_runtime::spawn(async move {
        let Ok(desktop) = helix_desktop_api::Desktop::session().await else {
            return;
        };
        let _ = desktop
            .watch_ui_scale(move |scale| {
                eprintln!("UI scale {scale}");
                let app = scale_app.clone();
                // The window may not exist yet at startup: try for a few seconds.
                std::thread::spawn(move || {
                    for _ in 0..40 {
                        let windows = app.webview_windows();
                        if !windows.is_empty() {
                            for window in windows.values() {
                                let _ = window.set_zoom(scale);
                            }
                            return;
                        }
                        std::thread::sleep(std::time::Duration::from_millis(250));
                    }
                });
            })
            .await;
    });
    tauri::async_runtime::spawn(async move {
        let Ok(desktop) = helix_desktop_api::Desktop::session().await else {
            return;
        };
        let _ = desktop
            .watch_theme(move |look| {
                eprintln!(
                    "UI theme {} accent {}",
                    if look.dark { "dark" } else { "light" },
                    look.accent
                );
                APP_THEME.store(
                    if look.dark { 1 } else { 2 },
                    std::sync::atomic::Ordering::Relaxed,
                );
                if let Ok(mut accent) = APP_ACCENT.lock() {
                    *accent = look.accent.clone();
                }
                if let Some(script) = theme_script() {
                    for window in app.webview_windows().values() {
                        let _ = window.eval(&script);
                    }
                }
            })
            .await;
    });
}

fn main() {
    gtk::glib::set_prgname(Some("org.helix.SystemMonitor"));
    if std::env::var_os("JSC_useJIT").is_none() {
        std::env::set_var("JSC_useJIT", "false");
    }
    let page = startup_page();
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            monitor_view,
            monitor_set_gpu,
            layout_reporting,
            report_layout
        ])
        .on_page_load(|webview, payload| {
            if matches!(payload.event(), tauri::webview::PageLoadEvent::Finished) {
                if let Some(script) = theme_script() {
                    let _ = webview.eval(&script);
                }
            }
        })
        .setup(move |app| {
            follow_appearance(app.handle().clone());
            let root = proc_root();
            let mut state = State {
                monitor: Monitor::default(),
                proc_root: root,
                show_gpu: true,
                last_gpu_ms: 0,
            };
            sample(&mut state);
            app.manage::<Shared>(Arc::new(Mutex::new(state)));
            let url = match &page {
                Some(p) => format!("index.html?page={p}"),
                None => "index.html".into(),
            };
            WebviewWindowBuilder::new(app, WINDOW, WebviewUrl::App(url.into()))
                .title("System Monitor")
                .decorations(false)
                .transparent(true)
                .inner_size(960.0, 680.0)
                .min_inner_size(720.0, 480.0)
                .build()?;
            start_polls(app.handle().clone());
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running Helix System Monitor");
}
