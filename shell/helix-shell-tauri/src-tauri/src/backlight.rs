//! The screen's backlight: read from sysfs, written through logind (`SetBrightness`), which lets the person's own session do it without
//! root. Tests and the nested sessions set `HELIX_BACKLIGHT_DIR` to a directory standing in for the device (files `brightness` and
//! `max_brightness`), which is then read and written as plain files; the real system bus is never reached from them.

use std::path::PathBuf;

use helix_models::brightness::{Brightness, Request};

const CLASS_DIR: &str = "/sys/class/backlight";

/// The backlight device directory: the override, or the first device of the backlight class.
fn device_dir() -> Option<PathBuf> {
    if let Some(dir) = std::env::var_os("HELIX_BACKLIGHT_DIR") {
        return Some(PathBuf::from(dir));
    }
    let mut devices: Vec<PathBuf> = std::fs::read_dir(CLASS_DIR)
        .ok()?
        .flatten()
        .map(|entry| entry.path())
        .collect();
    devices.sort();
    devices.into_iter().next()
}

/// A few bytes of a sysfs file. sysfs reports a size of 4096 whatever a file holds, so the read is bounded by hand.
fn read_small(path: &std::path::Path) -> Option<String> {
    use std::io::Read;
    let mut text = String::new();
    std::fs::File::open(path)
        .ok()?
        .take(64)
        .read_to_string(&mut text)
        .ok()?;
    Some(text)
}

pub fn read() -> Option<Brightness> {
    let dir = device_dir()?;
    Brightness::parse(
        &read_small(&dir.join("brightness"))?,
        &read_small(&dir.join("max_brightness"))?,
    )
}

pub async fn write(request: Request) -> Result<(), String> {
    let dir = device_dir().ok_or("no backlight")?;
    if std::env::var_os("HELIX_BACKLIGHT_DIR").is_some() {
        return std::fs::write(dir.join("brightness"), request.value.to_string())
            .map_err(|e| e.to_string());
    }
    let name = dir
        .file_name()
        .and_then(|n| n.to_str())
        .ok_or("bad backlight name")?
        .to_string();
    let connection = zbus::Connection::system()
        .await
        .map_err(|e| e.to_string())?;
    let session = zbus::Proxy::new(
        &connection,
        "org.freedesktop.login1",
        "/org/freedesktop/login1/session/auto",
        "org.freedesktop.login1.Session",
    )
    .await
    .map_err(|e| e.to_string())?;
    session
        .call::<_, _, ()>(
            "SetBrightness",
            &("backlight", name.as_str(), request.value),
        )
        .await
        .map_err(|e| e.to_string())
}
