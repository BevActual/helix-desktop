//! Helix Software. Thin host: Flatpak (and fwupd) feed the pure `helix_software_model::software::Store`; the page draws sanitized views and
//! asks for confirmed mutations, which run as jobs with progress and Cancel. Nested tests set `HELIX_SOFTWARE_FAKE` to a JSON catalog and
//! never run real installs.

mod catalog;
mod rpm;

use std::collections::HashMap;
use std::io::{BufRead, BufReader, Read};
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use base64::Engine;
use catalog::{Info, Remote};
use helix_software_model::software::{
    valid_id, Discover, JobState, JobView, Jobs, Kind, Mutation, Package, PackageView, Store,
    STORE_KINDS,
};
use serde::Serialize;
use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindowBuilder};

const WINDOW: &str = "software";
const PAGES: [&str; 5] = ["discover", "explore", "library", "updates", "sources"];
/// Screenshots are fetched only from the remote's own media host, and only when a detail page asks.
const SCREENSHOT_HOSTS: [&str; 1] = ["https://dl.flathub.org/"];

struct State {
    store: Store,
    jobs: Jobs,
    fake: bool,
    /// What is waiting for Allow: a package id (`*` = every update) and what to do.
    pending: Option<(String, Mutation)>,
    loading: bool,
    checking: bool,
    error: String,
    remotes: Vec<Remote>,
    icons: HashMap<String, Option<String>>,
    fake_icons: HashMap<String, String>,
    fake_info: HashMap<String, Vec<String>>,
    fake_screens: HashMap<String, Vec<String>>,
    indexing: bool,
    /// The last appstream index, kept so a catalog read can re-attach it at once.
    meta: HashMap<String, helix_software_model::software::Meta>,
    cancels: HashMap<u64, Arc<AtomicBool>>,
    /// The Fedora applications of the catalog (desktop id, icon, origin) and the names behind the System software row.
    rpm_apps: HashMap<String, rpm::RpmApp>,
    rpm_system_names: Vec<String>,
}

type Shared = Arc<Mutex<State>>;

fn shared(app: &AppHandle) -> Shared {
    app.state::<Shared>().inner().clone()
}

fn changed(app: &AppHandle) {
    let _ = app.emit("software-changed", ());
}

fn startup_arg(name: &str) -> Option<String> {
    let mut args = std::env::args().skip(1);
    let long = format!("--{name}");
    let prefixed = format!("--{name}=");
    while let Some(arg) = args.next() {
        if let Some(value) = arg.strip_prefix(&prefixed) {
            return Some(value.to_string());
        }
        if arg == long {
            return args.next();
        }
    }
    None
}

fn startup_app() -> Option<String> {
    startup_arg("app").filter(|id| valid_id(id))
}

fn startup_page() -> Option<String> {
    startup_arg("page").filter(|page| PAGES.contains(&page.as_str()))
}

// ---- running Flatpak ----

fn flatpak(args: &[&str]) -> Result<String, String> {
    let output = Command::new("flatpak")
        .args(args)
        .env("LC_ALL", "C")
        .stdin(Stdio::null())
        .output()
        .map_err(|error| format!("Flatpak is not available: {error}"))?;
    if output.status.success() {
        Ok(String::from_utf8_lossy(&output.stdout).into_owned())
    } else {
        let message = String::from_utf8_lossy(&output.stderr);
        Err(message
            .lines()
            .rev()
            .find(|l| !l.trim().is_empty())
            .unwrap_or("Flatpak failed.")
            .trim()
            .to_string())
    }
}

/// Reads the catalog from Flatpak's local caches, merges installed and updates, and stores the result. Never touches the network.
fn read_catalog(state: &Shared, app: &AppHandle) {
    {
        let mut guard = state.lock().expect("state");
        guard.loading = true;
        guard.error.clear();
    }
    changed(app);
    let flatpak_on = std::env::var_os("HELIX_SOFTWARE_NO_FLATPAK").is_none();
    const COLUMNS: &str = "--columns=application,name,description,version,origin,download-size";
    // Each installation is read on its own: one remote without a cached summary must not hide the rest, or the installed apps.
    let mut remote_text = String::new();
    let mut update_text = String::new();
    let mut remote_error = String::new();
    for scope in ["--user", "--system"].into_iter().filter(|_| flatpak_on) {
        match flatpak(&[scope, "remote-ls", "--cached", "--app", COLUMNS]) {
            Ok(text) => remote_text.push_str(&text),
            Err(error) => remote_error = error,
        }
        if let Ok(text) = flatpak(&[
            scope,
            "remote-ls",
            "--cached",
            "--updates",
            "--app",
            "--columns=application",
        ]) {
            update_text.push_str(&text);
        }
    }
    let installed = if flatpak_on {
        flatpak(&[
            "list",
            "--app",
            "--columns=application,name,version,origin,installation,size",
        ])
    } else {
        Err("Flatpak is switched off.".to_string())
    };
    let remotes = if flatpak_on {
        flatpak(&["remotes", "--columns=name,title,url,options"])
    } else {
        Err(String::new())
    };
    // Fedora's applications, through PackageKit. Where PackageKit is not there (another distribution) the store is Flatpak only.
    let rpm = read_rpm();
    if let Err(error) = &rpm {
        eprintln!("SOFTWARE rpm backend unavailable: {error}");
    }
    let mut guard = state.lock().expect("state");
    guard.loading = false;
    match installed {
        Ok(installed) => {
            let remotes = remotes
                .map(|text| catalog::parse_remotes(&text))
                .unwrap_or_default();
            let user_remotes: Vec<String> = remotes
                .iter()
                .filter(|r| r.installation == "user")
                .map(|r| r.name.clone())
                .collect();
            let mut packages = catalog::merge(
                catalog::parse_remote_ls(&remote_text),
                catalog::parse_list(&installed),
                &catalog::parse_updates(&update_text),
                &user_remotes,
            );
            packages.extend(firmware_updates());
            if let Ok((catalog, system)) = rpm {
                packages.extend(catalog.packages);
                guard.rpm_apps = catalog.apps;
                guard.rpm_system_names = system;
            }
            guard.store.set(packages);
            let kept = guard.meta.clone();
            guard.store.apply_meta(&kept);
            guard.remotes = remotes;
            guard.icons.clear();
            if remote_text.trim().is_empty() && !remote_error.is_empty() {
                eprintln!("SOFTWARE catalog error: {remote_error}");
                guard.error = format!("The software catalog is not available yet ({remote_error}). Use Check for Updates when online.");
            }
        }
        Err(error) => {
            if let Ok((catalog, system)) = rpm {
                // No Flatpak (or switched off): the Fedora catalog is the store.
                let mut packages = catalog.packages;
                packages.extend(firmware_updates());
                guard.store.set(packages);
                guard.rpm_apps = catalog.apps;
                guard.rpm_system_names = system;
                guard.remotes = Vec::new();
                guard.icons.clear();
            } else {
                eprintln!("SOFTWARE catalog error: {error}");
                guard.error = format!("Could not read the software catalog: {error}");
            }
        }
    }
    drop(guard);
    changed(app);
    build_index(state, app);
}

/// The Fedora catalog merged with what PackageKit says is installed and updatable, and the names behind the System software row.
fn read_rpm() -> Result<(rpm::RpmCatalog, Vec<String>), String> {
    let apps = rpm::read_catalog();
    let packagekit = rpm::PackageKit::connect()?;
    let installed = packagekit.installed()?;
    // Updates need the repository metadata; without it the catalog is still right about what is installed.
    let updates = packagekit.updates().unwrap_or_default();
    let catalog = rpm::build(apps, &installed, &updates);
    let system = rpm::system_update_names(&updates, &catalog.apps);
    Ok((catalog, system))
}

/// Reads the appstream documents of the configured remotes once and attaches categories, release dates and verification to the catalog,
/// which is what Discover and the category pages are made from. Runs on the catalog thread, after the list is already on screen.
fn build_index(state: &Shared, app: &AppHandle) {
    let remotes = {
        let mut guard = state.lock().expect("state");
        if guard.fake || guard.indexing || !guard.meta.is_empty() {
            return;
        }
        guard.indexing = true;
        guard.remotes.clone()
    };
    let mut meta = HashMap::new();
    for remote in &remotes {
        if remote.name.contains('/') || remote.name.contains("..") {
            continue;
        }
        let path = catalog::installation_base(&remote.installation)
            .join("appstream")
            .join(&remote.name)
            .join(catalog::arch())
            .join("active/appstream.xml.gz");
        if !path.is_file() {
            continue;
        }
        let Ok(mut child) = Command::new("zcat")
            .arg(&path)
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
        else {
            continue;
        };
        if let Some(stdout) = child.stdout.take() {
            for (id, found) in catalog::index_appstream(BufReader::new(stdout)) {
                meta.entry(id).or_insert(found);
            }
        }
        let _ = child.wait();
    }
    let mut guard = state.lock().expect("state");
    guard.indexing = false;
    guard.store.apply_meta(&meta);
    guard.meta = meta;
    drop(guard);
    changed(app);
}

/// Firmware updates fwupd already knows about (best effort; empty when fwupd is not running).
fn firmware_updates() -> Vec<Package> {
    let Ok(output) = Command::new("fwupdmgr")
        .args(["get-updates", "--json", "--no-authenticate"])
        .stdin(Stdio::null())
        .output()
    else {
        return Vec::new();
    };
    if !output.status.success() {
        return Vec::new();
    }
    let Ok(value) = serde_json::from_slice::<serde_json::Value>(&output.stdout) else {
        return Vec::new();
    };
    value
        .get("Devices")
        .and_then(|d| d.as_array())
        .into_iter()
        .flatten()
        .take(40)
        .enumerate()
        .map(|(i, device)| Package {
            id: format!("fwupd-{i}"),
            title: device
                .get("Name")
                .and_then(|v| v.as_str())
                .unwrap_or("Firmware")
                .to_string(),
            summary: "Firmware update".into(),
            kind: Kind::Firmware,
            installed: true,
            update: true,
            remote: "fwupd".into(),
            ..Default::default()
        })
        .collect()
}

fn refresh_in_background(app: &AppHandle, network: bool) {
    let app = app.clone();
    std::thread::spawn(move || {
        let state = shared(&app);
        if network {
            {
                let mut guard = state.lock().expect("state");
                guard.checking = true;
            }
            changed(&app);
            // Fetch fresh appstream and summaries; a failure (offline) leaves the cached catalog in place.
            let result = if std::env::var_os("HELIX_SOFTWARE_NO_FLATPAK").is_some() {
                Ok(String::new())
            } else {
                flatpak(&["update", "--appstream", "-y", "--noninteractive"])
            };
            // Fedora's repositories, through PackageKit (absent on other distributions: then there is nothing to refresh).
            let refreshed =
                rpm::PackageKit::connect().and_then(|packagekit| packagekit.refresh(None));
            let mut guard = state.lock().expect("state");
            guard.checking = false;
            if result.is_ok() {
                guard.meta.clear();
            }
            if let Err(error) = result {
                guard.error = format!("Could not check for updates: {error}");
                drop(guard);
                changed(&app);
            } else if let Err(error) = refreshed {
                eprintln!("SOFTWARE rpm refresh: {error}");
                guard.error = format!("Could not refresh the Fedora repositories: {error}");
                drop(guard);
                changed(&app);
            }
        }
        let had_error = !state.lock().expect("state").error.is_empty();
        let kept = if had_error && network {
            Some(state.lock().expect("state").error.clone())
        } else {
            None
        };
        read_catalog(&state, &app);
        if let Some(message) = kept {
            let mut guard = state.lock().expect("state");
            if guard.error.is_empty() {
                guard.error = message;
            }
            drop(guard);
            changed(&app);
        }
    });
}

// ---- views ----

#[derive(Serialize)]
struct PendingView {
    id: String,
    title: String,
    action: &'static str,
    kind: Kind,
}

#[derive(Serialize)]
struct PageView {
    rows: Vec<PackageView>,
    total: usize,
    pending: Option<PendingView>,
    sources: Vec<Remote>,
    update_count: usize,
    installed_count: usize,
    loading: bool,
    checking: bool,
    error: String,
    jobs: Vec<JobView>,
    discover: Option<Discover>,
    /// The appstream index is still being read (Discover and categories fill in when it finishes).
    indexing: bool,
}

fn action_name(mutation: Mutation) -> &'static str {
    match mutation {
        Mutation::Install => "install",
        Mutation::Remove => "remove",
        Mutation::Update => "update",
    }
}

#[tauri::command]
fn software_view(
    app: AppHandle,
    page: String,
    query: String,
    category: String,
    offset: usize,
    limit: usize,
) -> PageView {
    let shared = shared(&app);
    let state = shared.lock().expect("state");
    let limit = limit.clamp(1, 200);
    let needle = query.trim().to_ascii_lowercase();
    let mut discover = None;
    let (rows, total) = match page.as_str() {
        // Discover with a query is a search over everything; without one it is the curated page.
        "discover" if needle.is_empty() => {
            let day = std::env::var("HELIX_SOFTWARE_FAKE_DAY")
                .ok()
                .and_then(|v| v.parse().ok())
                .unwrap_or_else(|| {
                    std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .map(|d| d.as_secs() / 86_400)
                        .unwrap_or(0)
                });
            let fake = state.fake;
            let has_icon = |p: &Package| {
                fake || match state.rpm_apps.get(&p.id).filter(|_| p.kind == Kind::Rpm) {
                    Some(app) => rpm::icon_paths(app).iter().any(|path| path.is_file()),
                    None => catalog::icon_paths(&p.remote, &p.id, &p.installation)
                        .iter()
                        .any(|path| path.is_file()),
                }
            };
            let found = state.store.discover(&STORE_KINDS, day, 12, &has_icon);
            if std::env::var_os("HELIX_SOFTWARE_DEBUG").is_some() {
                eprintln!(
                    "SOFTWARE discover hero={} shelves={:?}",
                    found.hero.is_some(),
                    found
                        .shelves
                        .iter()
                        .map(|s| (s.title.clone(), s.rows.len()))
                        .collect::<Vec<_>>()
                );
            }
            discover = Some(found);
            (Vec::new(), 0)
        }
        "discover" | "explore" | "category" => {
            let in_category = if page == "category" {
                category.as_str()
            } else {
                ""
            };
            let page = state
                .store
                .explore_in(&STORE_KINDS, &query, in_category, offset, limit);
            (page.rows, page.total)
        }
        "library" => {
            let all: Vec<PackageView> = state
                .store
                .library(&STORE_KINDS)
                .into_iter()
                .filter(|p| {
                    needle.is_empty()
                        || p.title.to_ascii_lowercase().contains(&needle)
                        || p.id.to_ascii_lowercase().contains(&needle)
                })
                .collect();
            let total = all.len();
            (all.into_iter().skip(offset).take(limit).collect(), total)
        }
        "updates" => {
            let all = state.store.updates();
            let total = all.len();
            (all, total)
        }
        _ => (Vec::new(), 0),
    };
    let pending = state.pending.as_ref().map(|(id, mutation)| {
        let detail = state.store.detail(id);
        PendingView {
            title: if id == "*" {
                "all updates".to_string()
            } else {
                detail
                    .as_ref()
                    .map(|p| p.title.clone())
                    .filter(|t| !t.is_empty())
                    .unwrap_or_else(|| id.clone())
            },
            kind: detail.map(|p| p.kind).unwrap_or(Kind::Flatpak),
            id: id.clone(),
            action: action_name(*mutation),
        }
    });
    PageView {
        rows,
        total,
        pending,
        sources: if page == "sources" {
            state.remotes.clone()
        } else {
            Vec::new()
        },
        update_count: state.store.updates().len(),
        installed_count: state.store.library(&STORE_KINDS).len(),
        loading: state.loading,
        checking: state.checking,
        error: state.error.clone(),
        jobs: state.jobs.view(),
        discover,
        indexing: state.indexing,
    }
}

#[tauri::command]
fn software_detail(app: AppHandle, id: String) -> Option<PackageView> {
    let shared = shared(&app);
    let state = shared.lock().expect("state");
    state.store.detail(&id)
}

#[derive(Serialize)]
struct InfoView {
    #[serde(flatten)]
    info: Info,
    found: bool,
}

/// The long description, developer, licence and categories from the appstream data of the package's remote.
#[tauri::command]
async fn software_info(app: AppHandle, id: String) -> InfoView {
    let (package, fake_text, rpm_app) = {
        let shared = shared(&app);
        let state = shared.lock().expect("state");
        (
            state.store.detail(&id),
            state.fake_info.get(&id).cloned(),
            state.rpm_apps.get(&id).cloned(),
        )
    };
    if let Some(description) = fake_text {
        return InfoView {
            info: Info {
                developer: "Helix Test".into(),
                license: "MIT".into(),
                description,
                categories: vec!["Utility".into()],
                ..Default::default()
            },
            found: true,
        };
    }
    let Some(package) = package else {
        return InfoView {
            info: Info::default(),
            found: false,
        };
    };
    tauri::async_runtime::spawn_blocking(move || {
        if package.kind == Kind::Rpm {
            // A Fedora application: its description is in the distribution's catalog.
            let block = rpm_app.as_ref().and_then(rpm::find_block);
            return match block {
                Some(block) => InfoView {
                    info: catalog::parse_component(&block),
                    found: true,
                },
                None => InfoView {
                    info: Info::default(),
                    found: false,
                },
            };
        }
        let order: [&str; 2] = if package.installation == "system" {
            ["system", "user"]
        } else {
            ["user", "system"]
        };
        for which in order {
            let path = catalog::installation_base(which)
                .join("appstream")
                .join(&package.remote)
                .join(catalog::arch())
                .join("active/appstream.xml.gz");
            if !path.is_file() || package.remote.contains('/') {
                continue;
            }
            if let Some(block) = find_component(&path, &package.id) {
                return InfoView {
                    info: catalog::parse_component(&block),
                    found: true,
                };
            }
        }
        InfoView {
            info: Info::default(),
            found: false,
        }
    })
    .await
    .unwrap_or(InfoView {
        info: Info::default(),
        found: false,
    })
}

/// Streams the decompressed appstream XML and returns the `<component>` block whose `<id>` is `id`.
fn find_component(path: &PathBuf, id: &str) -> Option<String> {
    let mut child = Command::new("zcat")
        .arg(path)
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .ok()?;
    let reader = BufReader::new(child.stdout.take()?);
    let wanted = format!("<id>{id}</id>");
    let wanted_desktop = format!("<id>{id}.desktop</id>");
    let mut block = String::new();
    let mut inside = false;
    let mut hit = false;
    let mut found = None;
    for raw in reader.lines().map_while(Result::ok) {
        for line in catalog::split_components(&raw) {
            if line.trim_start().starts_with("<component") {
                inside = true;
                hit = false;
                block.clear();
            }
            if inside {
                block.push_str(&line);
                block.push('\n');
                if line.contains(&wanted) || line.contains(&wanted_desktop) {
                    hit = true;
                }
                if line.trim_start().starts_with("</component>") {
                    inside = false;
                    if hit {
                        found = Some(std::mem::take(&mut block));
                        break;
                    }
                }
            }
        }
        if found.is_some() {
            break;
        }
    }
    let _ = child.kill();
    let _ = child.wait();
    found
}

#[derive(Serialize)]
struct ScreenView {
    caption: String,
    uri: String,
}

fn image_uri(bytes: &[u8]) -> Option<String> {
    let mime = if bytes.starts_with(b"\x89PNG") {
        "image/png"
    } else if bytes.starts_with(&[0xFF, 0xD8]) {
        "image/jpeg"
    } else if bytes.starts_with(b"RIFF") && bytes.get(8..12) == Some(b"WEBP") {
        "image/webp"
    } else {
        return None;
    };
    Some(format!(
        "data:{mime};base64,{}",
        base64::engine::general_purpose::STANDARD.encode(bytes)
    ))
}

/// An application's screenshots, fetched on demand from the remote's media host and cached on disk. Anything that is not an image, is
/// over 3 MB, or is not on the allowed host is dropped.
#[tauri::command]
async fn software_screenshots(app: AppHandle, id: String) -> Vec<ScreenView> {
    let (package, fake) = {
        let shared = shared(&app);
        let state = shared.lock().expect("state");
        (
            state.store.detail(&id),
            state.fake_screens.get(&id).cloned(),
        )
    };
    if let Some(list) = fake {
        return list
            .into_iter()
            .map(|uri| ScreenView {
                caption: String::new(),
                uri,
            })
            .collect();
    }
    // Only Flatpak remotes publish screenshots from a host this app is willing to fetch from.
    let Some(package) = package.filter(|p| p.kind == Kind::Flatpak) else {
        return Vec::new();
    };
    tauri::async_runtime::spawn_blocking(move || {
        let mut shots = Vec::new();
        let order: [&str; 2] = if package.installation == "system" {
            ["system", "user"]
        } else {
            ["user", "system"]
        };
        for which in order {
            let path = catalog::installation_base(which)
                .join("appstream")
                .join(&package.remote)
                .join(catalog::arch())
                .join("active/appstream.xml.gz");
            if package.remote.contains('/') || !path.is_file() {
                continue;
            }
            if let Some(block) = find_component(&path, &package.id) {
                shots = catalog::parse_component(&block).screenshots;
                break;
            }
        }
        let cache = std::env::var_os("XDG_CACHE_HOME")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".cache")))
            .unwrap_or_default()
            .join("helix/software/screens");
        let _ = std::fs::create_dir_all(&cache);
        let mut out = Vec::new();
        for (index, shot) in shots.into_iter().take(5).enumerate() {
            if !SCREENSHOT_HOSTS
                .iter()
                .any(|host| shot.url.starts_with(host))
                || shot.url.contains("..")
            {
                continue;
            }
            let file = cache.join(format!("{}-{}.img", package.id, index));
            let bytes = match std::fs::read(&file) {
                Ok(bytes) if !bytes.is_empty() => bytes,
                _ => {
                    let ok = Command::new("curl")
                        .args([
                            "-fsSL",
                            "--max-time",
                            "12",
                            "--max-filesize",
                            "3000000",
                            "-o",
                        ])
                        .arg(&file)
                        .arg(&shot.url)
                        .stdin(Stdio::null())
                        .stderr(Stdio::null())
                        .status()
                        .map(|s| s.success())
                        .unwrap_or(false);
                    if !ok {
                        let _ = std::fs::remove_file(&file);
                        continue;
                    }
                    std::fs::read(&file).unwrap_or_default()
                }
            };
            if bytes.len() > 3_000_000 {
                continue;
            }
            if let Some(uri) = image_uri(&bytes) {
                out.push(ScreenView {
                    caption: shot.caption,
                    uri,
                });
            }
        }
        out
    })
    .await
    .unwrap_or_default()
}

fn icon_uri(bytes: &[u8]) -> String {
    format!(
        "data:image/png;base64,{}",
        base64::engine::general_purpose::STANDARD.encode(bytes)
    )
}

/// An application's icon as a data URI, from the appstream icons of its remote (None → the page draws a lettered tile).
#[tauri::command]
fn software_icon(app: AppHandle, id: String) -> Option<String> {
    let shared = shared(&app);
    let mut state = shared.lock().expect("state");
    if let Some(cached) = state.icons.get(&id) {
        return cached.clone();
    }
    if let Some(uri) = state.fake_icons.get(&id) {
        return Some(uri.clone());
    }
    let package = state.store.detail(&id)?;
    let candidates = match state
        .rpm_apps
        .get(&id)
        .filter(|_| package.kind == Kind::Rpm)
    {
        Some(app) => rpm::icon_paths(app),
        None => catalog::icon_paths(&package.remote, &package.id, &package.installation),
    };
    let found = candidates
        .into_iter()
        .find_map(|path| {
            std::fs::read(path)
                .ok()
                .filter(|bytes| bytes.len() < 400_000)
        })
        .map(|bytes| icon_uri(&bytes));
    state.icons.insert(id, found.clone());
    found
}

// ---- mutations ----

#[tauri::command]
fn software_request(app: AppHandle, id: String, action: String) -> Result<(), String> {
    let mutation = match action.as_str() {
        "install" => Mutation::Install,
        "remove" => Mutation::Remove,
        "update" => Mutation::Update,
        _ => return Err("Unknown action.".into()),
    };
    let shared = shared(&app);
    let mut state = shared.lock().expect("state");
    if state.jobs.running() && (id == "*" || state.jobs.is_running_for(&id)) {
        return Err("That is already in progress.".into());
    }
    if id == "*" {
        if mutation != Mutation::Update || state.store.updates().is_empty() {
            return Err("There is nothing to update.".into());
        }
    } else {
        state
            .store
            .prepare(&id, mutation)
            .map_err(|e| format!("{e:?}"))?;
    }
    state.pending = Some((id, mutation));
    drop(state);
    changed(&app);
    Ok(())
}

fn last_percent(chunk: &str) -> Option<u8> {
    let bytes = chunk.as_bytes();
    let mut at = bytes.len();
    while let Some(i) = chunk[..at].rfind('%') {
        let digits: String = chunk[..i]
            .chars()
            .rev()
            .take_while(|c| c.is_ascii_digit())
            .collect::<Vec<_>>()
            .into_iter()
            .rev()
            .collect();
        if let Ok(value) = digits.parse::<u16>() {
            if value <= 100 {
                return Some(value as u8);
            }
        }
        at = i;
    }
    None
}

#[tauri::command]
fn software_confirm(app: AppHandle, accept: bool) -> Result<(), String> {
    let state_handle = shared(&app);
    let (id, mutation, job, cancel, plan) = {
        let mut state = state_handle.lock().expect("state");
        let Some((id, mutation)) = state.pending.take() else {
            return Ok(());
        };
        if !accept {
            drop(state);
            changed(&app);
            return Ok(());
        }
        let package = if id == "*" {
            None
        } else {
            state.store.detail(&id)
        };
        let title = package
            .as_ref()
            .map(|p| p.title.clone())
            .unwrap_or_else(|| "All updates".into());
        let plan = plan_command(&state, &id, mutation, package.as_ref())?;
        let job = state.jobs.start(&id, &title, mutation);
        let cancel = Arc::new(AtomicBool::new(false));
        state.cancels.insert(job, cancel.clone());
        (id, mutation, job, cancel, plan)
    };
    changed(&app);
    let app_for_worker = app.clone();
    std::thread::spawn(move || run_job(app_for_worker, job, id, mutation, plan, cancel));
    Ok(())
}

/// The backend command for a mutation; chosen from what the store knows, never from the page.
fn plan_command(
    state: &State,
    id: &str,
    mutation: Mutation,
    package: Option<&PackageView>,
) -> Result<Plan, String> {
    if state.fake {
        return Ok(Plan::Fake);
    }
    let s = |args: &[&str]| args.iter().map(|a| a.to_string()).collect::<Vec<_>>();
    if id == "*" {
        // Every kind that has something to update, in turn; a failure stops the rest.
        let has = |kind: Kind| state.store.updates().iter().any(|p| p.kind == kind);
        let mut steps = Vec::new();
        if has(Kind::Flatpak) {
            steps.push(Step::Command(s(&[
                "flatpak",
                "update",
                "-y",
                "--noninteractive",
            ])));
        }
        if has(Kind::Rpm) {
            steps.push(Step::Rpm(rpm::Op::Update(None)));
        }
        return Ok(Plan::Steps(steps));
    }
    let package = package.ok_or("Unknown package.")?;
    if package.kind == Kind::Firmware {
        return Ok(Plan::Steps(vec![Step::Command(s(&[
            "fwupdmgr",
            "update",
            "-y",
            "--no-reboot-check",
        ]))]));
    }
    if package.kind == Kind::Rpm {
        let op = match mutation {
            Mutation::Install => rpm::Op::Install(package.id.clone()),
            Mutation::Remove => rpm::Op::Remove(package.id.clone()),
            Mutation::Update if package.id == rpm::SYSTEM_UPDATES => {
                rpm::Op::Update(Some(state.rpm_system_names.clone()))
            }
            Mutation::Update => rpm::Op::Update(Some(vec![package.id.clone()])),
        };
        return Ok(Plan::Steps(vec![Step::Rpm(op)]));
    }
    let scope = |installation: &str| {
        if installation == "user" {
            "--user"
        } else {
            "--system"
        }
    };
    Ok(Plan::Steps(vec![Step::Command(match mutation {
        Mutation::Install => {
            // A user installation of the remote needs no authentication, so it wins.
            let user = state
                .remotes
                .iter()
                .any(|r| r.name == package.remote && r.installation == "user");
            let system = state
                .remotes
                .iter()
                .any(|r| r.name == package.remote && r.installation == "system");
            let installation = if user {
                "user"
            } else if system {
                "system"
            } else {
                return Err("That source is no longer configured.".into());
            };
            vec![
                "flatpak".into(),
                "install".into(),
                "-y".into(),
                "--noninteractive".into(),
                scope(installation).into(),
                package.remote.clone(),
                package.id.clone(),
            ]
        }
        Mutation::Remove => vec![
            "flatpak".into(),
            "uninstall".into(),
            "-y".into(),
            "--noninteractive".into(),
            scope(&package.installation).into(),
            package.id.clone(),
        ],
        Mutation::Update => vec![
            "flatpak".into(),
            "update".into(),
            "-y".into(),
            "--noninteractive".into(),
            scope(&package.installation).into(),
            package.id.clone(),
        ],
    })]))
}

/// What a confirmed change runs: the page never builds a command, the store's own knowledge of the package does.
enum Plan {
    /// The fake catalog of the nested tests.
    Fake,
    Steps(Vec<Step>),
}

enum Step {
    /// A Flatpak or fwupd command line.
    Command(Vec<String>),
    /// A change made through PackageKit.
    Rpm(rpm::Op),
}

/// Runs the fake backend: a few visible steps so the page can be tested.
fn run_fake(
    app: &AppHandle,
    job: u64,
    id: &str,
    mutation: Mutation,
    cancel: &Arc<AtomicBool>,
) -> (JobState, String) {
    let state = shared(app);
    // Fake backend: a few visible steps so the page can be tested.
    let delay = std::env::var("HELIX_SOFTWARE_FAKE_STEP_MS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(250);
    let mut outcome = (JobState::Done, String::new());
    for step in 1..=4u8 {
        std::thread::sleep(Duration::from_millis(delay));
        if cancel.load(Ordering::SeqCst) {
            outcome = (JobState::Cancelled, "Cancelled.".into());
            break;
        }
        state
            .lock()
            .expect("state")
            .jobs
            .progress(job, step * 25 - 5);
        changed(app);
    }
    if outcome.0 == JobState::Done && std::env::var_os("HELIX_SOFTWARE_FAKE_FAIL").is_some() {
        outcome = (JobState::Failed, "The remote could not be reached.".into());
    }
    if outcome.0 == JobState::Done {
        eprintln!("SOFTWARE fake {:?} {id}", mutation);
    }
    outcome
}

/// Runs a command line, parsing its progress, until it ends or the job is cancelled.
fn run_command(
    app: &AppHandle,
    job: u64,
    plan: &[String],
    cancel: &Arc<AtomicBool>,
) -> (JobState, String) {
    let spawned = Command::new(&plan[0])
        .args(&plan[1..])
        .env("LC_ALL", "C")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn();
    match spawned {
        Err(error) => (JobState::Failed, error.to_string()),
        Ok(mut child) => {
            let tail = Arc::new(Mutex::new(String::new()));
            let mut readers = Vec::new();
            for stream in [
                child
                    .stdout
                    .take()
                    .map(|s| Box::new(s) as Box<dyn Read + Send>),
                child
                    .stderr
                    .take()
                    .map(|s| Box::new(s) as Box<dyn Read + Send>),
            ]
            .into_iter()
            .flatten()
            {
                let app = app.clone();
                let tail = tail.clone();
                readers.push(std::thread::spawn(move || {
                    let mut stream = stream;
                    let mut buffer = [0u8; 2048];
                    while let Ok(n) = stream.read(&mut buffer) {
                        if n == 0 {
                            break;
                        }
                        let text = String::from_utf8_lossy(&buffer[..n]).into_owned();
                        if let Some(percent) = last_percent(&text) {
                            shared(&app)
                                .lock()
                                .expect("state")
                                .jobs
                                .progress(job, percent);
                            changed(&app);
                        }
                        let mut tail = tail.lock().expect("tail");
                        tail.push_str(&text);
                        if tail.len() > 4000 {
                            let cut = tail.len() - 2000;
                            *tail = tail[cut..].to_string();
                        }
                    }
                }));
            }
            let status = loop {
                if cancel.load(Ordering::SeqCst) {
                    let _ = child.kill();
                    let _ = child.wait();
                    break None;
                }
                match child.try_wait() {
                    Ok(Some(status)) => break Some(status),
                    Ok(None) => std::thread::sleep(Duration::from_millis(120)),
                    Err(_) => break None,
                }
            };
            for reader in readers {
                let _ = reader.join();
            }
            match status {
                None => (JobState::Cancelled, "Cancelled.".into()),
                Some(status) if status.success() => (JobState::Done, String::new()),
                Some(_) => {
                    let tail = tail.lock().expect("tail");
                    let last = tail
                        .split(['\n', '\r'])
                        .rev()
                        .map(str::trim)
                        .find(|l| !l.is_empty() && !l.ends_with('%'))
                        .unwrap_or("The command failed.");
                    (
                        JobState::Failed,
                        last.trim_start_matches("error: ").to_string(),
                    )
                }
            }
        }
    }
}

/// Runs a PackageKit change, with its progress on the job.
fn run_rpm(
    app: &AppHandle,
    job: u64,
    op: &rpm::Op,
    cancel: &Arc<AtomicBool>,
) -> (JobState, String) {
    let packagekit = match rpm::PackageKit::connect() {
        Ok(packagekit) => packagekit,
        Err(error) => return (JobState::Failed, error),
    };
    let result = packagekit.run(op, cancel, |percent| {
        shared(app)
            .lock()
            .expect("state")
            .jobs
            .progress(job, percent);
        changed(app);
    });
    match result {
        Ok(()) => (JobState::Done, String::new()),
        Err(message) if cancel.load(Ordering::SeqCst) && message == "Cancelled." => {
            (JobState::Cancelled, message)
        }
        Err(message) => (JobState::Failed, message),
    }
}

fn run_job(
    app: AppHandle,
    job: u64,
    id: String,
    mutation: Mutation,
    plan: Plan,
    cancel: Arc<AtomicBool>,
) {
    let state = shared(&app);
    let (state_result, message) = match &plan {
        Plan::Fake => run_fake(&app, job, &id, mutation, &cancel),
        Plan::Steps(steps) => {
            let mut outcome = (JobState::Done, String::new());
            for step in steps {
                outcome = match step {
                    Step::Command(command) => run_command(&app, job, command, &cancel),
                    Step::Rpm(op) => run_rpm(&app, job, op, &cancel),
                };
                if outcome.0 != JobState::Done {
                    break;
                }
            }
            outcome
        }
    };
    {
        let mut guard = state.lock().expect("state");
        guard.cancels.remove(&job);
        if state_result == JobState::Failed {
            eprintln!("SOFTWARE job {id} failed: {message}");
        }
        guard.jobs.finish(job, state_result, &message);
        if state_result == JobState::Done && id != "*" {
            let _ = guard.store.apply(&id, mutation);
        }
        guard.icons.remove(&id);
    }
    changed(&app);
    if state_result == JobState::Done && !state.lock().expect("state").fake {
        // Resync with what Flatpak now reports (versions, sizes, other updates).
        read_catalog(&state, &app);
    } else if state_result == JobState::Done && id == "*" {
        let mut guard = state.lock().expect("state");
        let ids: Vec<String> = guard.store.updates().into_iter().map(|p| p.id).collect();
        for id in ids {
            let _ = guard.store.apply(&id, Mutation::Update);
        }
        drop(guard);
        changed(&app);
    }
}

#[tauri::command]
fn software_cancel(app: AppHandle, job: u64) {
    let shared = shared(&app);
    let state = shared.lock().expect("state");
    if let Some(flag) = state.cancels.get(&job) {
        flag.store(true, Ordering::SeqCst);
    }
}

#[tauri::command]
fn software_dismiss(app: AppHandle, job: u64) {
    shared(&app).lock().expect("state").jobs.dismiss(job);
    changed(&app);
}

#[tauri::command]
fn software_open(app: AppHandle, id: String) -> Result<(), String> {
    let shared = shared(&app);
    let state = shared.lock().expect("state");
    let package = state.store.detail(&id).ok_or("Unknown application.")?;
    if !package.installed || package.kind == Kind::Firmware || !valid_id(&id) {
        return Err("That application is not installed.".into());
    }
    if state.fake {
        eprintln!("SOFTWARE fake open {id}");
        return Ok(());
    }
    let mut launch = Command::new("flatpak");
    if package.kind == Kind::Rpm {
        // A desktop entry the catalog named for this package, started the way a menu would.
        let desktop = state
            .rpm_apps
            .get(&id)
            .map(|app| app.desktop_id.trim_end_matches(".desktop").to_string())
            .filter(|name| valid_id(name))
            .ok_or("That application has no launcher.")?;
        launch = Command::new("gtk-launch");
        launch.arg(desktop);
    } else {
        launch.args(["run", &id]);
    }
    let mut child = launch
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|error| error.to_string())?;
    std::thread::spawn(move || {
        let _ = child.wait();
    });
    Ok(())
}

/// Re-reads Flatpak's caches (and, with `network`, fetches fresh appstream first).
#[tauri::command]
fn software_refresh(app: AppHandle, network: bool) {
    let fake = shared(&app).lock().expect("state").fake;
    if fake {
        changed(&app);
        return;
    }
    refresh_in_background(&app, network);
}

// ---- fake catalog ----

fn load_fake(path: &PathBuf, state: &mut State) {
    let Ok(text) = std::fs::read_to_string(path) else {
        return;
    };
    let Ok(value) = serde_json::from_str::<serde_json::Value>(&text) else {
        return;
    };
    let mut packages = Vec::new();
    for row in value.as_array().into_iter().flatten() {
        let Some(id) = row.get("id").and_then(|v| v.as_str()) else {
            continue;
        };
        let text = |key: &str| {
            row.get(key)
                .and_then(|v| v.as_str())
                .unwrap_or("")
                .to_string()
        };
        if let Some(icon) = row.get("icon").and_then(|v| v.as_str()) {
            state.fake_icons.insert(id.to_string(), icon.to_string());
        }
        if let Some(list) = row.get("screenshots").and_then(|v| v.as_array()) {
            state.fake_screens.insert(
                id.to_string(),
                list.iter()
                    .filter_map(|l| l.as_str().map(str::to_string))
                    .collect(),
            );
        }
        if let Some(lines) = row.get("description").and_then(|v| v.as_array()) {
            state.fake_info.insert(
                id.to_string(),
                lines
                    .iter()
                    .filter_map(|l| l.as_str().map(str::to_string))
                    .collect(),
            );
        }
        packages.push(Package {
            id: id.to_string(),
            title: text("title"),
            summary: text("summary"),
            kind: match row
                .get("kind")
                .and_then(|v| v.as_str())
                .unwrap_or("flatpak")
            {
                "rpm" => Kind::Rpm,
                "firmware" => Kind::Firmware,
                _ => Kind::Flatpak,
            },
            installed: row
                .get("installed")
                .and_then(|v| v.as_bool())
                .unwrap_or(false),
            update: row.get("update").and_then(|v| v.as_bool()).unwrap_or(false),
            remote: text("remote"),
            version: text("version"),
            size: row.get("size").and_then(|v| v.as_u64()).unwrap_or(0),
            installation: String::new(),
            categories: row
                .get("categories")
                .and_then(|v| v.as_array())
                .map(|list| {
                    list.iter()
                        .filter_map(|c| c.as_str().map(str::to_string))
                        .collect()
                })
                .unwrap_or_default(),
            released: row.get("released").and_then(|v| v.as_u64()).unwrap_or(0),
            update_only: row
                .get("update_only")
                .and_then(|v| v.as_bool())
                .unwrap_or(false),
            verified: row
                .get("verified")
                .and_then(|v| v.as_bool())
                .unwrap_or(false),
        });
    }
    state.store.set(packages);
    state.remotes = vec![
        Remote {
            name: "flathub".into(),
            title: "Flathub".into(),
            url: "https://dl.flathub.org/repo/".into(),
            installation: "user".into(),
            enabled: true,
        },
        Remote {
            name: "fedora".into(),
            title: "Fedora Flatpaks".into(),
            url: "oci+https://registry.fedoraproject.org".into(),
            installation: "system".into(),
            enabled: true,
        },
    ];
    state.fake = true;
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

/// 0 until the desktop service has said, then 1 for dark and 2 for light.
static APP_THEME: std::sync::atomic::AtomicU8 = std::sync::atomic::AtomicU8::new(0);

fn theme_script() -> Option<&'static str> {
    match APP_THEME.load(std::sync::atomic::Ordering::Relaxed) {
        1 => Some("document.documentElement.dataset.theme = 'dark'"),
        2 => Some("document.documentElement.dataset.theme = 'light'"),
        _ => None,
    }
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
            .watch_theme_dark(move |dark| {
                eprintln!("UI theme {}", if dark { "dark" } else { "light" });
                APP_THEME.store(
                    if dark { 1 } else { 2 },
                    std::sync::atomic::Ordering::Relaxed,
                );
                if let Some(script) = theme_script() {
                    for window in app.webview_windows().values() {
                        let _ = window.eval(script);
                    }
                }
            })
            .await;
    });
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
            software_info,
            software_icon,
            software_screenshots,
            software_request,
            software_confirm,
            software_cancel,
            software_dismiss,
            software_open,
            software_refresh,
            layout_reporting,
            report_layout
        ])
        .on_page_load(|webview, payload| {
            if matches!(payload.event(), tauri::webview::PageLoadEvent::Finished) {
                if let Some(script) = theme_script() {
                    let _ = webview.eval(script);
                }
            }
        })
        .setup(move |app| {
            follow_appearance(app.handle().clone());
            let mut state = State {
                store: Store::default(),
                jobs: Jobs::default(),
                fake: false,
                pending: None,
                loading: false,
                checking: false,
                error: String::new(),
                remotes: Vec::new(),
                icons: HashMap::new(),
                fake_icons: HashMap::new(),
                fake_info: HashMap::new(),
                fake_screens: HashMap::new(),
                indexing: false,
                meta: HashMap::new(),
                cancels: HashMap::new(),
                rpm_apps: HashMap::new(),
                rpm_system_names: Vec::new(),
            };
            let fake_path = std::env::var_os("HELIX_SOFTWARE_FAKE").map(PathBuf::from);
            if let Some(path) = &fake_path {
                load_fake(path, &mut state);
                eprintln!("SOFTWARE fake catalog {}", path.display());
            }
            let is_fake = state.fake;
            app.manage::<Shared>(Arc::new(Mutex::new(state)));
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
            if !is_fake {
                refresh_in_background(app.handle(), false);
            }
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running Helix Software");
}
