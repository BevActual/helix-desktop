//! Automatic location. When the person turns on `location.auto_locate` (Settings → Date & time), this asks GeoClue (the system's location service)
//! where this computer is and keeps `location.latitude`, `location.longitude` and `location.address` up to date, so the automatic theme and night
//! light find sunrise and sunset by themselves. It is opt-in, and the answer is rounded to about a kilometre and asked for at city accuracy, which is
//! all sunrise needs. GeoClue decides where the answer comes from (a network lookup, a GPS, a modem) and whether this program may ask at all: the
//! package installs `/etc/geoclue/conf.d/90-helix.conf` for that. What happened is written to `$XDG_STATE_HOME/helix/location-status.json` for Settings.

use std::sync::Arc;
use std::time::Duration;

use futures_util::StreamExt;
use serde_json::json;
use tokio::sync::Mutex;
use zbus::zvariant::{ObjectPath, OwnedObjectPath};
use zbus::{Connection, Proxy};

use crate::state::State;

const SERVICE: &str = "org.freedesktop.GeoClue2";
const DESKTOP_ID: &str = "org.helix.Desktop";
/// GeoClue's "city" accuracy level.
const CITY: u32 = 4;
/// Metres the computer must move before GeoClue says again.
const DISTANCE_THRESHOLD: u32 = 50_000;

fn status_path() -> Option<std::path::PathBuf> {
    let base = std::env::var_os("XDG_STATE_HOME")
        .filter(|v| !v.is_empty())
        .map(std::path::PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME").map(|h| std::path::PathBuf::from(h).join(".local/state"))
        })?;
    Some(base.join("helix/location-status.json"))
}

/// What Settings shows: `state` is one of `off`, `locating`, `located`, `denied`, `unavailable`.
fn write_status(state: &str, message: &str) {
    let Some(path) = status_path() else { return };
    if let Some(dir) = path.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let text = json!({"state": state, "message": message}).to_string();
    let temporary = path.with_extension("json.tmp");
    if std::fs::write(&temporary, text).is_ok() {
        let _ = std::fs::rename(&temporary, &path);
    }
}

/// Two decimals is about a kilometre: the place, not the street.
fn rounded(degrees: f64) -> f64 {
    (degrees * 100.0).round() / 100.0
}

/// Why GeoClue said no, in words for the person.
fn explain(error: &zbus::Error) -> (&'static str, String) {
    let text = error.to_string();
    if text.contains("AccessDenied")
        || text.contains("not authorized")
        || text.contains("Not authorized")
    {
        (
            "denied",
            "The location service does not allow Helix to ask. Reinstall Helix, or add it to /etc/geoclue/conf.d, and try again.".into(),
        )
    } else if text.contains("ServiceUnknown")
        || text.contains("NameHasNoOwner")
        || text.contains("No such file")
    {
        (
            "unavailable",
            "This computer has no location service (GeoClue) running.".into(),
        )
    } else {
        (
            "unavailable",
            format!("The location service could not be used: {text}"),
        )
    }
}

struct Locator {
    client: Proxy<'static>,
    task: tokio::task::JoinHandle<()>,
}

impl Locator {
    async fn stop(self) {
        self.task.abort();
        let _ = self.client.call_method("Stop", &()).await;
    }
}

async fn start(state: Arc<Mutex<State>>, session: Connection) -> zbus::Result<Locator> {
    let system = Connection::system().await?;
    let manager = Proxy::new(
        &system,
        SERVICE,
        "/org/freedesktop/GeoClue2/Manager",
        "org.freedesktop.GeoClue2.Manager",
    )
    .await?;
    let path: OwnedObjectPath = manager.call("GetClient", &()).await?;
    let client = Proxy::new(&system, SERVICE, path, "org.freedesktop.GeoClue2.Client").await?;
    client.set_property("DesktopId", DESKTOP_ID).await?;
    client.set_property("RequestedAccuracyLevel", CITY).await?;
    client
        .set_property("DistanceThreshold", DISTANCE_THRESHOLD)
        .await?;
    let mut updates = client.receive_signal("LocationUpdated").await?;
    client.call_method("Start", &()).await?;
    write_status("locating", "Looking for where you are…");
    let watching = system.clone();
    let task = tokio::spawn(async move {
        while let Some(message) = updates.next().await {
            let body = message.body();
            let Ok((_old, new)) = body.deserialize::<(ObjectPath<'_>, ObjectPath<'_>)>() else {
                continue;
            };
            if new.as_str() == "/" {
                continue;
            }
            let new = new.to_owned();
            let Ok(location) =
                Proxy::new(&watching, SERVICE, new, "org.freedesktop.GeoClue2.Location").await
            else {
                continue;
            };
            let latitude: f64 = location.get_property("Latitude").await.unwrap_or(f64::NAN);
            let longitude: f64 = location.get_property("Longitude").await.unwrap_or(f64::NAN);
            let description: String = location
                .get_property("Description")
                .await
                .unwrap_or_default();
            if !latitude.is_finite() || !longitude.is_finite() {
                continue;
            }
            apply(
                &state,
                &session,
                rounded(latitude),
                rounded(longitude),
                &description,
            )
            .await;
        }
    });
    Ok(Locator { client, task })
}

async fn apply(
    state: &Arc<Mutex<State>>,
    session: &Connection,
    latitude: f64,
    longitude: f64,
    description: &str,
) {
    let place: String = description
        .chars()
        .filter(|c| !c.is_control())
        .take(120)
        .collect();
    {
        let mut locked = state.lock().await;
        locked.config.force_reload();
        let result = locked
            .config
            .set("location.latitude", json!(latitude))
            .and_then(|()| locked.config.set("location.longitude", json!(longitude)))
            .and_then(|()| locked.config.set("location.address", json!(place)));
        if let Err(error) = result {
            tracing::warn!("location: could not keep {latitude}, {longitude}: {error}");
            return;
        }
    }
    tracing::info!("location: {latitude}, {longitude} {place}");
    write_status(
        "located",
        &if place.is_empty() {
            format!("{latitude:.2}, {longitude:.2}")
        } else {
            place
        },
    );
    crate::audio_support::emit_configuration_changed(session).await;
}

/// Watches `location.auto_locate` and starts or stops asking GeoClue to match it.
pub async fn run(state: Arc<Mutex<State>>, session: Connection) {
    let mut active: Option<Locator> = None;
    let mut backoff_until: Option<tokio::time::Instant> = None;
    loop {
        let want = {
            let mut locked = state.lock().await;
            locked.config.force_reload();
            locked
                .config
                .get("location.auto_locate")
                .as_bool()
                .unwrap_or(false)
        };
        match (want, active.is_some()) {
            (true, false) if backoff_until.is_none_or(|at| tokio::time::Instant::now() >= at) => {
                match start(Arc::clone(&state), session.clone()).await {
                    Ok(locator) => active = Some(locator),
                    Err(error) => {
                        let (kind, message) = explain(&error);
                        tracing::warn!("location: {error}");
                        write_status(kind, &message);
                        // Not again for a minute: a refusal does not change by asking faster.
                        backoff_until = Some(tokio::time::Instant::now() + Duration::from_secs(60));
                    }
                }
            }
            (false, true) => {
                if let Some(locator) = active.take() {
                    locator.stop().await;
                }
                write_status("off", "");
                backoff_until = None;
            }
            (false, false) => backoff_until = None,
            _ => {}
        }
        tokio::time::sleep(Duration::from_secs(3)).await;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn places_are_rounded_to_about_a_kilometre() {
        assert_eq!(rounded(48.856_613), 48.86);
        assert_eq!(rounded(-0.127_758), -0.13);
    }
}
