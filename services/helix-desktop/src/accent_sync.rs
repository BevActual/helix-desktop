//! Lets GTK 4 and libadwaita applications follow the accent colour: whenever `theme.accent` changes, the matching name is written to GNOME's
//! `org.gnome.desktop.interface accent-color`, which those toolkits (and the desktop portal) read. Helix's own applications follow the setting directly.
//! Nothing is written until the person has chosen an accent, and nothing happens where `gsettings` or the key does not exist.

use std::process::Stdio;
use std::sync::Arc;
use std::time::Duration;

use tokio::process::Command;
use tokio::sync::Mutex;

use crate::state::State;

/// GNOME's name for a Helix accent (graphite is its slate); `None` for a name GNOME has no colour for.
pub fn gnome_name(accent: &str) -> Option<&'static str> {
    Some(match accent {
        "blue" => "blue",
        "purple" => "purple",
        "pink" => "pink",
        "red" => "red",
        "orange" => "orange",
        "yellow" => "yellow",
        "green" => "green",
        "graphite" => "slate",
        _ => return None,
    })
}

async fn write(name: &str) -> bool {
    Command::new("gsettings")
        .args(["set", "org.gnome.desktop.interface", "accent-color", name])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .await
        .is_ok_and(|status| status.success())
}

/// Follows `theme.accent` for as long as the service runs.
pub async fn run(state: Arc<Mutex<State>>) {
    let mut written: Option<&'static str> = None;
    let mut unsupported = false;
    loop {
        let wanted = {
            let mut locked = state.lock().await;
            locked.config.force_reload();
            locked
                .config
                .get("theme.accent")
                .as_str()
                .and_then(gnome_name)
        };
        if let (Some(name), false) = (wanted, unsupported) {
            if written != Some(name) {
                if write(name).await {
                    written = Some(name);
                } else {
                    // No gsettings, or no such key (an older GNOME): stop asking.
                    tracing::debug!(
                        "accent-color is not available; GTK applications keep their own accent"
                    );
                    unsupported = true;
                }
            }
        }
        tokio::time::sleep(Duration::from_secs(3)).await;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_helix_accent_has_a_gnome_name() {
        for name in helix_config::settings::ACCENTS {
            assert!(gnome_name(name).is_some(), "{name}");
        }
        assert_eq!(gnome_name("graphite"), Some("slate"));
        assert_eq!(gnome_name("teal"), None);
    }
}
