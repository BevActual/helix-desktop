//! The system tray: this shell is the StatusNotifierWatcher and the host. Applications register an item (a bus name and object path); the
//! shell reads what the item publishes, draws it in the bar, and forwards a click as `Activate`, `ContextMenu` or `SecondaryActivate`. What
//! is acceptable to register, and everything an application says, is decided by `helix_shell_core::tray`; the page can only name an item by
//! a key that model made. A name that is already owned by another watcher is never taken from it.

use std::collections::HashMap;
use std::sync::OnceLock;
use std::time::Duration;

use base64::Engine;
use futures_util::StreamExt;
use helix_shell_core::tray::{parse_service, Props, Registered, Status};
use serde::Serialize;
use tauri::{AppHandle, Emitter};
use zbus::zvariant::OwnedValue;

use crate::{apps, shell_state};

const WATCHER_NAME: &str = "org.kde.StatusNotifierWatcher";
const WATCHER_PATH: &str = "/StatusNotifierWatcher";
const ITEM_INTERFACE: &str = "org.kde.StatusNotifierItem";

static CONNECTION: OnceLock<zbus::Connection> = OnceLock::new();

struct Watcher {
    app: AppHandle,
}

#[zbus::interface(name = "org.kde.StatusNotifierWatcher")]
impl Watcher {
    async fn register_status_notifier_item(
        &self,
        service: String,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> zbus::fdo::Result<()> {
        let sender = header.sender().map(ToString::to_string).unwrap_or_default();
        let (bus, path) = parse_service(&service, &sender).ok_or_else(|| {
            zbus::fdo::Error::InvalidArgs("not a bus name and object path".into())
        })?;
        register(&self.app, bus, path).await;
        Ok(())
    }

    /// The shell is the only host; others may announce themselves but nothing changes.
    async fn register_status_notifier_host(&self, _service: String) {}

    #[zbus(property)]
    async fn registered_status_notifier_items(&self) -> Vec<String> {
        shell_state(&self.app)
            .lock()
            .expect("shell state")
            .tray
            .registered()
    }

    #[zbus(property)]
    async fn is_status_notifier_host_registered(&self) -> bool {
        true
    }

    #[zbus(property)]
    async fn protocol_version(&self) -> i32 {
        0
    }
}

#[derive(Serialize, Clone)]
struct PixmapJson {
    width: usize,
    height: usize,
    /// Straight RGBA, base64.
    rgba: String,
}

#[derive(Serialize, Clone)]
pub struct TrayItem {
    key: String,
    title: String,
    status: Status,
    icon_uri: Option<String>,
    pixmap: Option<PixmapJson>,
}

fn view(app: &AppHandle) -> Vec<TrayItem> {
    let state = shell_state(app);
    let shell = state.lock().expect("shell state");
    shell
        .tray
        .view()
        .into_iter()
        .map(|item| {
            let theme = shell.tray.icon_theme_path(&item.key);
            let icon_uri = (!item.icon_name.is_empty())
                .then(|| {
                    theme
                        .and_then(|dir| {
                            ["png", "svg"].iter().find_map(|ext| {
                                apps::icon_data_uri(&format!("{dir}/{}.{ext}", item.icon_name))
                            })
                        })
                        .or_else(|| apps::icon_data_uri(&item.icon_name))
                })
                .flatten();
            TrayItem {
                key: item.key,
                title: item.title,
                status: item.status,
                icon_uri,
                pixmap: item.pixmap.map(|p| PixmapJson {
                    width: p.width,
                    height: p.height,
                    rgba: base64::engine::general_purpose::STANDARD.encode(p.rgba),
                }),
            }
        })
        .collect()
}

fn publish(app: &AppHandle) {
    let items = view(app);
    eprintln!("TRAY view items={}", items.len());
    let _ = app.emit("tray-view", items);
}

async fn signal(name: &str, service: &str) {
    if let Some(connection) = CONNECTION.get() {
        let _ = connection
            .emit_signal(None::<&str>, WATCHER_PATH, WATCHER_NAME, name, &(service,))
            .await;
    }
}

async fn register(app: &AppHandle, bus: String, path: String) {
    let result = shell_state(app)
        .lock()
        .expect("shell state")
        .tray
        .register(&bus, &path);
    if result != Registered::Added {
        eprintln!("TRAY registration {result:?}");
        return;
    }
    let key = format!("{bus}{path}");
    signal("StatusNotifierItemRegistered", &key).await;
    let app = app.clone();
    tauri::async_runtime::spawn(async move { follow_item(app, bus, path, key).await });
}

fn text(props: &HashMap<String, OwnedValue>, key: &str) -> String {
    props
        .get(key)
        .and_then(|v| <String>::try_from(v.try_clone().ok()?).ok())
        .unwrap_or_default()
}

async fn read_props(connection: &zbus::Connection, bus: &str, path: &str) -> Option<Props> {
    let proxy = zbus::fdo::PropertiesProxy::builder(connection)
        .destination(bus.to_string())
        .ok()?
        .path(path.to_string())
        .ok()?
        .build()
        .await
        .ok()?;
    let all = proxy
        .get_all(zbus::names::InterfaceName::try_from(ITEM_INTERFACE).ok()?)
        .await
        .ok()?;
    let pixmaps = all
        .get("IconPixmap")
        .and_then(|v| <Vec<(i32, i32, Vec<u8>)>>::try_from(v.try_clone().ok()?).ok())
        .unwrap_or_default();
    Some(Props {
        id: text(&all, "Id"),
        title: text(&all, "Title"),
        icon_name: text(&all, "IconName"),
        icon_theme_path: text(&all, "IconThemePath"),
        status: text(&all, "Status"),
        pixmaps,
    })
}

/// Reads the item now and again after each of its change signals, until the application leaves.
async fn follow_item(app: AppHandle, bus: String, path: String, key: String) {
    let Some(connection) = CONNECTION.get() else {
        return;
    };
    let rule = zbus::MatchRule::builder()
        .msg_type(zbus::message::Type::Signal)
        .sender(bus.as_str())
        .and_then(|b| b.path(path.as_str()))
        .and_then(|b| b.interface(ITEM_INTERFACE))
        .map(|b| b.build());
    let Ok(rule) = rule else { return };
    let Ok(mut changes) = zbus::MessageStream::for_match_rule(rule, connection, Some(16)).await
    else {
        return;
    };
    loop {
        match read_props(connection, &bus, &path).await {
            Some(props) => {
                {
                    let state = shell_state(&app);
                    let mut shell = state.lock().expect("shell state");
                    if !shell.tray.knows(&key) {
                        return;
                    }
                    shell.tray.update(&key, &props);
                }
                publish(&app);
            }
            None => {
                // It registered but does not answer: drop it rather than show an item nothing can drive.
                forget(&app, &key).await;
                return;
            }
        }
        if changes.next().await.is_none() {
            return;
        }
        // Several signals usually come together (a new icon and a new title): read once for the burst.
        tokio::time::sleep(Duration::from_millis(80)).await;
        while tokio::time::timeout(Duration::from_millis(1), changes.next())
            .await
            .is_ok_and(|m| m.is_some())
        {}
    }
}

async fn forget(app: &AppHandle, key: &str) {
    if shell_state(app)
        .lock()
        .expect("shell state")
        .tray
        .remove(key)
    {
        signal("StatusNotifierItemUnregistered", key).await;
        publish(app);
    }
}

/// Removes the items of an application that left the bus.
async fn watch_departures(app: AppHandle) {
    let Some(connection) = CONNECTION.get() else {
        return;
    };
    let Ok(bus) = zbus::fdo::DBusProxy::new(connection).await else {
        return;
    };
    let Ok(mut changes) = bus.receive_name_owner_changed().await else {
        return;
    };
    while let Some(change) = changes.next().await {
        let Ok(args) = change.args() else { continue };
        if args.new_owner().is_some() {
            continue;
        }
        let gone = shell_state(&app)
            .lock()
            .expect("shell state")
            .tray
            .remove_bus_name(args.name().as_str());
        if !gone.is_empty() {
            for key in &gone {
                signal("StatusNotifierItemUnregistered", key).await;
            }
            publish(&app);
        }
    }
}

/// Claims the watcher name if nobody has it (another desktop's watcher is never displaced), and serves it for the life of the shell.
pub fn serve(app: AppHandle) {
    tauri::async_runtime::spawn(async move {
        let built = zbus::connection::Builder::session()
            .map(|b| {
                b.allow_name_replacements(false)
                    .replace_existing_names(false)
            })
            .and_then(|b| b.name(WATCHER_NAME))
            .and_then(|b| b.serve_at(WATCHER_PATH, Watcher { app: app.clone() }));
        let connection = match built {
            Ok(builder) => builder.build().await,
            Err(error) => Err(error),
        };
        match connection {
            Ok(connection) => {
                eprintln!("TRAY serving {WATCHER_NAME}");
                let _ = CONNECTION.set(connection);
                signal("StatusNotifierHostRegistered", "").await;
                publish(&app);
                watch_departures(app).await;
            }
            Err(error) => eprintln!("TRAY could not serve {WATCHER_NAME}: {error}"),
        }
    });
}

fn address(app: &AppHandle, key: &str) -> Result<(String, String), String> {
    let state = shell_state(app);
    let shell = state.lock().expect("shell state");
    shell
        .tray
        .address(key)
        .map(|(b, p)| (b.to_string(), p.to_string()))
        .ok_or_else(|| "not a tray item".to_string())
}

async fn call(app: &AppHandle, key: &str, method: &str, x: i32, y: i32) -> Result<(), String> {
    let (bus, path) = address(app, key)?;
    let connection = CONNECTION.get().ok_or("tray not running")?;
    let proxy = zbus::Proxy::new(connection, bus, path, ITEM_INTERFACE)
        .await
        .map_err(|e| e.to_string())?;
    eprintln!("TRAY {method}");
    proxy
        .call::<_, _, ()>(method, &(x, y))
        .await
        .map_err(|e| e.to_string())
}

#[tauri::command]
pub fn tray_view(app: AppHandle) -> Vec<TrayItem> {
    view(&app)
}

#[tauri::command]
pub async fn tray_activate(app: AppHandle, key: String, x: i32, y: i32) -> Result<(), String> {
    call(&app, &key, "Activate", x, y).await
}

#[tauri::command]
pub async fn tray_context(app: AppHandle, key: String, x: i32, y: i32) -> Result<(), String> {
    call(&app, &key, "ContextMenu", x, y).await
}

#[tauri::command]
pub async fn tray_secondary(app: AppHandle, key: String, x: i32, y: i32) -> Result<(), String> {
    call(&app, &key, "SecondaryActivate", x, y).await
}
