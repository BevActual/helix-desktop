//! The power mode (Power saver, Balanced, Performance) from power-profiles-daemon, over the system bus, at the top of Power & lock. Reading and changing
//! it needs no password on a normal desktop. A machine without the daemon (or with one profile) shows no row.

use std::collections::HashMap;

use helix_config::catalog::{Choice, Control, RowView};
use zbus::zvariant::OwnedValue;

/// The daemon has had two names; the newer one is tried first.
const NAMES: [(&str, &str); 2] = [
    (
        "org.freedesktop.UPower.PowerProfiles",
        "/org/freedesktop/UPower/PowerProfiles",
    ),
    ("net.hadess.PowerProfiles", "/net/hadess/PowerProfiles"),
];

/// What the daemon says: the active profile and the ones it offers, in its order.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Profiles {
    pub active: String,
    pub offered: Vec<String>,
}

async fn proxy() -> Result<zbus::Proxy<'static>, String> {
    let connection = zbus::Connection::system()
        .await
        .map_err(|e| e.to_string())?;
    let mut last = String::from("no power profiles service");
    for (name, path) in NAMES {
        match zbus::Proxy::new(&connection, name, path, name).await {
            // A proxy is made without asking the service, so ask for something to know it is there.
            Ok(proxy) => match proxy.get_property::<String>("ActiveProfile").await {
                Ok(_) => return Ok(proxy),
                Err(error) => last = error.to_string(),
            },
            Err(error) => last = error.to_string(),
        }
    }
    Err(last)
}

pub async fn read() -> Option<Profiles> {
    let proxy = proxy().await.ok()?;
    let active: String = proxy.get_property("ActiveProfile").await.ok()?;
    let list: Vec<HashMap<String, OwnedValue>> = proxy.get_property("Profiles").await.ok()?;
    let offered: Vec<String> = list
        .iter()
        .filter_map(|entry| entry.get("Profile")?.downcast_ref::<String>().ok())
        .collect();
    Some(Profiles { active, offered })
}

pub async fn set(profile: &str) -> Result<(), String> {
    let current = read()
        .await
        .ok_or("power modes are not available on this computer")?;
    if !current.offered.iter().any(|p| p == profile) {
        return Err("that power mode is not offered".into());
    }
    proxy()
        .await?
        .set_property("ActiveProfile", profile)
        .await
        .map_err(|e| e.to_string())
}

fn label(profile: &str) -> String {
    match profile {
        "power-saver" => "Power saver".into(),
        "balanced" => "Balanced".into(),
        "performance" => "Performance".into(),
        other => other.replace('-', " "),
    }
}

/// The row, or none when there is nothing to choose between.
pub fn rows(profiles: &Profiles) -> Vec<RowView> {
    if profiles.offered.len() < 2 {
        return Vec::new();
    }
    vec![RowView {
        id: "profile:mode".into(),
        title: "Power mode".into(),
        group: "Power mode".into(),
        description: "Power saver stretches the battery and keeps the fans quiet. Performance runs as fast as it can.".into(),
        overridden: false,
        control: Control::Choice {
            options: profiles
                .offered
                .iter()
                .map(|p| Choice { value: p.clone(), label: label(p) })
                .collect(),
            current: profiles.active.clone(),
        },
    }]
}

#[cfg(test)]
mod tests {
    use super::*;

    fn offered(names: &[&str]) -> Profiles {
        Profiles {
            active: "balanced".into(),
            offered: names.iter().map(|s| s.to_string()).collect(),
        }
    }

    #[test]
    fn a_choice_only_when_there_is_something_to_choose() {
        assert!(rows(&offered(&["balanced"])).is_empty());
        assert!(rows(&offered(&[])).is_empty());
        let rows = rows(&offered(&["power-saver", "balanced", "performance"]));
        match &rows[0].control {
            Control::Choice { options, current } => {
                assert_eq!(current, "balanced");
                assert_eq!(options[0].label, "Power saver");
                assert_eq!(options[2].label, "Performance");
            }
            _ => panic!("a choice"),
        }
        assert_eq!(label("low-power"), "low power");
    }

    /// Against the real daemon on this computer: `cargo test -p helix-settings powerprofile -- --ignored --nocapture`.
    #[test]
    #[ignore]
    fn the_real_daemon_answers() {
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .unwrap();
        let profiles = runtime.block_on(read());
        println!("{profiles:?}");
        assert!(profiles.is_some());
        // Setting the mode it already has is harmless and shows the write path works.
        let current = profiles.unwrap().active;
        assert_eq!(runtime.block_on(set(&current)), Ok(()));
    }
}
