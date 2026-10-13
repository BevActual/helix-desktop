//! The D-Bus contract in `protocols/dbus/org.helix.Desktop.xml`. Names here are checked against that
//! file by the crate tests. Agent Surface is exposed only as the confirmer role
//! ([`crate::CONFIRMER_METHODS`] / [`crate::CONFIRMER_SIGNALS`]); the shell must not hold grants or
//! talk to the model. `helix-assistant` is the client that does.

use std::collections::BTreeMap;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Interface {
    pub methods: Vec<String>,
    pub signals: Vec<String>,
}

/// Bus name and object path, the same constants as `src/desktop_api/protocol.h`.
pub const BUS_NAME: &str = "org.helix.Desktop";
pub const OBJECT_PATH: &str = "/org/helix/Desktop";

pub fn parse_interfaces(xml: &str) -> BTreeMap<String, Interface> {
    let mut interfaces = BTreeMap::<String, Interface>::new();
    let mut current: Option<String> = None;
    for raw in xml.lines() {
        let line = raw.trim();
        if let Some(name) = attribute(line, "interface") {
            current = Some(name.to_string());
            interfaces
                .entry(name.to_string())
                .or_insert_with(|| Interface {
                    methods: Vec::new(),
                    signals: Vec::new(),
                });
        }
        let Some(iface) = current.as_deref() else {
            continue;
        };
        if line.starts_with("</interface") {
            current = None;
            continue;
        }
        if let Some(name) = attribute(line, "method") {
            if let Some(interface) = interfaces.get_mut(iface) {
                interface.methods.push(name.to_string());
            }
        } else if let Some(name) = attribute(line, "signal") {
            if let Some(interface) = interfaces.get_mut(iface) {
                interface.signals.push(name.to_string());
            }
        }
    }
    interfaces
}

fn attribute<'a>(line: &'a str, element: &str) -> Option<&'a str> {
    let start = line.find('<')?;
    let rest = line[start + 1..].trim_start();
    if !rest.starts_with(element) {
        return None;
    }
    let after = &rest[element.len()..];
    if !after.starts_with(|c: char| c.is_ascii_whitespace() || c == '/' || c == '>') {
        return None;
    }
    let key = "name=\"";
    let at = line.find(key)? + key.len();
    let end = line[at..].find('"')? + at;
    Some(&line[at..end])
}

pub fn protocol_xml() -> &'static str {
    include_str!("../../../protocols/dbus/org.helix.Desktop.xml")
}

#[cfg(test)]
mod tests {
    use super::{parse_interfaces, protocol_xml, BUS_NAME, OBJECT_PATH};

    #[test]
    fn names_match_protocol_header() {
        let header = include_str!("../../../src/desktop_api/protocol.h");
        assert!(header.contains(&format!("\"{BUS_NAME}\"")));
        assert!(header.contains(&format!("\"{OBJECT_PATH}\"")));
    }

    #[test]
    fn xml_matches_the_rust_catalog() {
        let parsed = parse_interfaces(protocol_xml());
        let catalog: &[(&str, &[&str], &[&str])] = &[
            (
                "org.helix.Desktop.Displays1",
                &["GetSnapshot"],
                &["Changed"],
            ),
            (
                "org.helix.Desktop.Configuration1",
                &[
                    "GetSnapshot",
                    "GetSettings",
                    "Update",
                    "UpdateOrderedList",
                    "UpdateApplicationList",
                    "Reset",
                    "CommitOverrides",
                    "RegisterDesktop",
                    "RegisterCapabilities",
                    "GetState",
                    "SetState",
                    "ClearState",
                ],
                &["Changed"],
            ),
            (
                "org.helix.Desktop.Appearance1",
                &[
                    "GetSnapshot",
                    "Reapply",
                    "RefreshPalettes",
                    "SavePalette",
                    "SetRole",
                    "SetAccent",
                    "SetWallpaper",
                    "SearchWallhaven",
                    "GetWallhaven",
                    "DownloadWallhaven",
                    "CancelWallhaven",
                    "SetWallhavenApiKey",
                ],
                &["Applied", "Changed"],
            ),
            (
                "org.helix.Desktop.Audio1",
                &[
                    "StartInputMeter",
                    "StopInputMeter",
                    "GetSnapshot",
                    "Action",
                    "Cancel",
                ],
                &["InputLevel", "Changed", "Completed"],
            ),
            (
                "org.helix.Desktop.Network1",
                &[
                    "GetSnapshot",
                    "Action",
                    "GetWifiSecret",
                    "GetDns",
                    "SetDns",
                    "Reply",
                    "Cancel",
                ],
                &["Changed", "Prompt", "Completed"],
            ),
            (
                "org.helix.Desktop.Bluetooth1",
                &["GetSnapshot", "Action", "Reply", "Cancel"],
                &["Changed", "Prompt", "Completed"],
            ),
            (
                "org.helix.Desktop.AgentSurface1",
                &[
                    "Hello",
                    "RequestAccess",
                    "GetSnapshot",
                    "ListTools",
                    "Action",
                    "Cancel",
                    "RegisterConfirmer",
                    "Confirm",
                    "RegisterManager",
                    "GetGrants",
                    "SetGrant",
                    "RevokeClient",
                    "GetAudit",
                ],
                &[
                    "Completed",
                    "Changed",
                    "ConfirmationRequested",
                    "ConfirmationEnded",
                ],
            ),
            (
                "org.helix.Desktop.AgentSurfaceActivity1",
                &[
                    "GetActivity",
                    "ListPending",
                    "Stop",
                    "StopAll",
                    "GetGrants",
                    "RevokeClient",
                    "GetEnabled",
                    "SetEnabled",
                ],
                &["Changed", "PendingChanged"],
            ),
        ];
        assert_eq!(parsed.len(), catalog.len());
        for (name, methods, signals) in catalog {
            let interface = parsed
                .get(*name)
                .unwrap_or_else(|| panic!("missing {name}"));
            assert_eq!(
                interface.methods,
                methods.iter().map(|m| (*m).to_string()).collect::<Vec<_>>(),
                "{name}"
            );
            assert_eq!(
                interface.signals,
                signals.iter().map(|m| (*m).to_string()).collect::<Vec<_>>(),
                "{name}"
            );
        }
    }
}
